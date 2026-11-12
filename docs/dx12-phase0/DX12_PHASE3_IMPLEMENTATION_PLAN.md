# DX12 Phase 3 — Implementation Plan: Two-List Recorder and Resource-State Tracker

Status: Phase 3 plan only. No code, no build, no default changes.
Generated: 2026-10-26 (UTC), from the Phase 2 command structure and a read-only pass over
the live tree.
Inputs: `DX12_PHASE2_COMMAND_STRUCTURE.md` (two lists, 42 handoffs), Phase 1 slices, Phase
0 inventory.
Output folder: `F:\PinyonShift\dx12-phase0`.

## 0. Scope

Implement, behind `fh1_d3d11_command_lists` (default **off**):

1. A **frame list tape** (T_frame) recording S3 surface work, S7 draws with S6 state, S4
   resolves and S2/S4 transfers, replayed in order onto the single immediate context.
2. A **swap list tape** (T_swap) recording the S5 swap-texture upload, replayed after the
   frame fence and the readback.
3. A **resource-state tracker** that records the Phase 2 transitions and validates them;
   transitions are no-ops on D3D11 but stay in the tape for the native mapping.
4. The **submission queue and fence** that turn the plan's `F_frame`/`F_swap`/`F_present`
   values into the existing event query on D3D11.
5. Unit and integration tests that prove the off path is unchanged and the on path is
   byte-identical on the resolve/upload measurements.

Non-goals: native DX12 execution, a taped P2 paint path, shader-pack work, Phase 13
changes, and flipping any default. P1 and P2 remain immediate-context sections as Phase 2
specified.

## 1. Implementation decision: semantic packet tape

Two ways to record were considered.

**A. API proxy.** Replace `ID3D11DeviceContext*` with a proxy exposing the context
methods; forward when off, append calls when on. Rejected for Phase 3:
- Hundreds of context call sites across 13 files, plus hidden state in the caches.
- `Map`/`Unmap` cannot be recorded as GPU commands; CPU writes would still need a
  snapshot mechanism.
- D3D11's sticky state makes a replay that does not rebind everything fragile.
- Resource creation is interleaved with context calls and must stay CPU-immediate.

**B. Semantic packet tape (chosen).** Record at the points where the backend already
knows the complete pass: `IssueDraw`, `IssueCopy`, `IssueSwap`, texture cache upload and
mirror upload. A packet is a self-contained description of one pass: resources, shader
objects, constant bytes, views, states, draw/dispatch args. Replay rebinds everything,
then issues the one GPU call. This matches the D3D12 backend's deferred-tape idea
(`deferred_command_list.h:30-36`) while staying at the backend's semantic level, and it
avoids proxying the whole context.

The cost is a refactor of the three existing execute paths into "prepare packet" and
"execute packet" halves. The benefit is that the tape is finite, auditable, and testable
without a device.

## 2. Component architecture

```
D3D11CommandProcessor
  |-- IssueDraw  -- prepare --> [DrawPacket] --------.
  |-- IssueCopy  -- prepare --> [DispatchPacket,     |
  |                            CopyPacket, Barrier]  |
  |-- texture/mirror upload --> [UploadPacket]       |
  |                                                  v
  |                                      D3D11CommandList (T_frame / T_swap)
  |                                      - packet vector + byte arena
  |                                      - COM refs
  |                                      - packet/data caps
  |                                                  |
  |                                      D3D11ResourceStateTracker
  |                                      - per-resource state
  |                                      - transition/UAV barrier packets
  |                                                  |
  |                                      D3D11SubmissionQueue
  |                                      - replay under one mutex
  |                                      - one event query, fence values
  |                                      - deferred release, tape slots
  v                                                  v
ID3D11DeviceContext  <---------------------------- replay
```

### 2.1 Packet types

| Packet | Contents | Replay action |
|---|---|---|
| `Draw` | VS/PS/GS, topology, index buffer + offset + format, five constant-buffer slots with inline bytes, SRVs, samplers, RTVs/DSV, viewport, scissor, blend/depth-stencil/rasterizer states, blend factor, stencil ref, draw args | Bind all state, `Draw`/`DrawIndexed` |
| `Dispatch` | CS, constant bytes, source SRVs, destination UAV, group counts, `unbind_om` flag | Optional `OMSetRenderTargets(null)`, bind CS/CB/SRV/UAV, `Dispatch`, unbind |
| `Copy` | Source/destination resource, box or byte offset, length | `CopySubresourceRegion`/`CopyResource` |
| `UploadBuffer` | Mirror offset, bytes (tape arena) | Map staging, memcpy, unmap, copy to mirror |
| `UploadTexture` | Texture, subresource, box, bytes (tape arena) | Map/memcpy staging or `UpdateSubresource` |
| `Clear` | RTV/DSV, colour or depth/stencil values | `ClearRenderTargetView`/`ClearDepthStencilView` |
| `ResolveSubresource` | MSAA source, destination, format | `ResolveSubresource` |
| `Barrier` | Resource, old state (validation), new state | No-op on D3D11; tracker validates |
| `UavBarrier` | Resource | No-op on D3D11; tracker validates |

No `Present`, no `Map`, no `Flush` packet. Present is P2; Map is a replay-time action;
`Flush` stays an immediate queue operation.

### 2.2 Tape and arena

- One `D3D11CommandList` per open list, with `std::vector<Packet>` and a byte arena for
  inline data. Packets store `(arena_offset, size)` for constants, uploads and texture
  data; the arena is aligned to 16 bytes.
- Packet and data caps: 32,768 packets and 64 MiB per tape. On overflow, **submit the
  current tape at that point** and continue in a new tape; never execute the remainder
  immediately while a tape is pending. This is a safety-valve split, additional to Phase
  2's refresh split; it is counted and must not occur in the gate. The overflow is logged
  once per session.
- Every packet holds `ComPtr` references to the resources, views, shaders and states it
  names. The tape releases them when reset, which happens only after its fence value
  completes.
- Three tape slots per list type, matching the three-buffer swap chain. A slot is
  reusable only after the fence value it was submitted with has completed.
- The tape owns no device objects that outlive the command processor.

### 2.3 Replay rules

1. Replay is single-threaded under the submission mutex, in packet order.
2. Every packet rebinds every state it depends on. No packet may rely on a bind left by
   the previous packet. This is the D3D11 equivalent of a command list's state reset and
   removes the sticky-state failure mode.
3. Barrier packets are consumed by the tracker (already updated at record time) and emit
   nothing; a validation build checks that the replay's requested state matches.
4. `Map` failures, device removal or a failed draw/copy abort the replay, mark the frame
   failed, and report once through the existing device-loss path. The swap path skips P1
   and P2 for that frame and presents the previous output; no partial frame is published.
5. After replay, SRVs and UAVs that were bound by the last packet are unbound in debug
   validation mode only; the immediate P1/P2 sections set the state they need.
6. Fresh-resource creation clears (the EDRAM surface registry) remain immediate. A
   resource cannot reorder against packets that predate its existence, and its clear must
   only precede its own first use. It is registered as a render target in the tracker.
7. Any diagnostic that reads GPU content mid-frame calls `DrainForDiagnostic()`: submit
   the open tape, wait for its value, read, then open a new tape. Diagnostics never read
   a partially recorded frame.

### 2.4 Resource-state tracker

- Own enum mirroring the Phase 2 subset: `kCommon`, `kRenderTarget`, `kDepthWrite`,
  `kShaderResource`, `kUnorderedAccess`, `kCopySource`, `kCopyDest`, `kPresent`,
  `kResolveSource`, `kResolveDest`, `kIndexBuffer`, `kVertexBuffer`. No D3D12 headers in
  the D3D11 backend.
- API: `Register(resource, state)`, `Transition(list, resource, state)`,
  `UavBarrier(list, resource)`, `Forget(resource)`, `Validate(resource, state)`.
- `Transition` appends a `Barrier` packet only when the state changes; it updates the
  tracker at record time so later packets see the correct state.
- Unknown resources are auto-registered as `kCommon` and counted. A validation run must
  show zero unknowns except the swap-chain and presenter resources, which are registered
  explicitly at first use.
- Tracker lifetime: process-lifetime, cleared at shutdown; resources are forgotten when
  destroyed. No per-page states; buffers are whole-resource, textures whole-resource
  (conservative), matching Phase 2.

### 2.5 Submission queue and fence

- One `ID3D11DeviceContext`, one mutex, one `D3D11_QUERY_EVENT`, one monotonic value.
- `Submit(list)` replays under the mutex, calls `End(query)`, returns the new value.
- `Wait(value)` spins on `GetData` with a bounded sleep and checks
  `GetDeviceRemovedReason` on failure.
- `WaitAll()`, `DrainForDiagnostic()`, `Signal()` for the immediate P2 section.
- Deferred-release list of `(fence_value, ComPtr)` pairs, reclaimed on wait or the next
  submit.
- The D3D11 path is synchronous on the CPU; the values are bookkeeping that maps 1:1 to
  `ID3D12Fence` on the native path.
- When the flag is on, the queue owns its own event query; the command processor's
  existing `gpu_wait_query_`/`WaitForGpu` remains the off path's implementation and is
  not modified. When off, the queue is not constructed and none of these calls occur.

### 2.6 Default-off guarantee

- Cvar: `fh1_d3d11_command_lists`, bool, default false, group GPU, description naming the
  Phase 2 structure, `.lifecycle(kRequiresRestart)`. Environment variable
  `REX_FH1_D3D11_COMMAND_LISTS`.
- A second flag, `fh1_d3d11_command_list_validate`, default false, enables transition and
  replay assertions. It is a diagnostic and has no effect when the main flag is off.
- When the main flag is off:
  - no tape, tracker or queue is constructed;
  - `IssueDraw`, `IssueCopy` and `IssueSwap` call the existing execute code;
  - the only added cost is one cached bool check per pass;
  - the off path must remain byte-identical (trace-short md5 unchanged).
- The flag is read once in `SetupContext`; changing it requires a restart, matching the
  existing D3D11 flags.

## 3. Integration points

| File | Change | Risk |
|---|---|---|
| `include/rex/graphics/flags.h`, `src/graphics/flags.cpp` | Declare/define the two cvars next to the existing D3D11 resolve flags | Low |
| `src/graphics/CMakeLists.txt` | Add the new D3D11 sources to `REXGPU_XENOS_D3D11_SOURCES` | Low |
| `include/rex/graphics/d3d11/command_list.h`, `src/.../command_list.cpp` | New: packet definitions, arena, append/replay interface | Medium |
| `include/rex/graphics/d3d11/resource_state_tracker.h`, `src/.../resource_state_tracker.cpp` | New: state table, transition/UAV barrier emission, validation | Medium |
| `include/rex/graphics/d3d11/submission_queue.h`, `src/.../submission_queue.cpp` | New: mutex, event query, fence values, deferred release, tape slots | Medium |
| `include/rex/graphics/d3d11/command_processor.h`, `src/.../command_processor.cpp` | Open/close T_frame, route `IssueDraw`/`IssueCopy`/`IssueSwap`, own the components | High |
| `src/graphics/d3d11/fh1_native_resolve.cpp/.h` | Split `ResolveToMemory` into packet preparation and execution | Medium |
| `src/graphics/d3d11/fh1_native_executor.cpp` | Same for word-transfer dispatches; bind from packet state | Medium |
| `src/graphics/d3d11/texture_cache.cpp/.h` | Route `UpdateSubresource` uploads through the recorder; swap upload into T_swap | High |
| `src/graphics/d3d11/shared_memory_upload_device.cpp/.h` | Record mirror uploads as `UploadBuffer` packets (or expose a queue the recorder drains) | High |
| `src/graphics/d3d11/primitive_processor.cpp` | Verify the index ring's record-time writes are safe; capture buffer/offset in the draw packet | Low |
| `tests/unit/CMakeLists.txt`, `tests/unit/graphics/...` | Add command-list and tracker tests; extend the D3D11 test group | Low |

Files are snapshotted to `.musebak-20261026` before editing, per the no-git rule. All
edits stay inside `F:\PinyonShift\work\.local\rexglue`, including its `tests` tree; only
the notes file lives outside it, under `.local`.

## 4. Milestones

Each milestone is a buildable, revertible unit. `M0`–`M1` change no runtime behaviour.

### M0 — Flag and scaffolding (small)

Deliver:
- cvars and build wiring;
- empty `D3D11CommandList`, `D3D11ResourceStateTracker`, `D3D11SubmissionQueue` with
  constructors/destructors and compiling stubs that do no work;
- command processor owns them but never calls them when the flag is off;
- one-time log when the flag is on but the recording path is not wired yet.

Accept:
- `.local\muse_build_game.bat` → `BUILD_EXIT=0`;
- `unit_tests "d3d11*"` passes;
- trace-short on defaults → md5 `b8f68e89c6192577747ab452aa5598ef`;
- flag on produces the "not wired" log and otherwise the default frame.

### M1 — Tape and tracker core with unit tests (medium)

**Status (2026-10-26): complete for the packet stream, arena, COM references,
replay sink and state-tracker map. Queue value tests are deferred with the
queue itself to M4. See `DX12_PHASE3_M1_RESULT.md`.**

Deliver:
- packet encoding, arena, COM refs, replay sink;
- tracker transitions, coalescing, validation;
- tests with a recording sink (no device) that assert order, rebinding, ref counts,
  packet/data caps, barrier coalescing and unknown-resource handling;
- queue value tests with a fake completion source.

Accept:
- all new unit tests pass; existing `d3d11*` tests pass;
- no runtime path uses the tape yet;
- flag-off trace-short unchanged.

### M2 — Resolve and transfer recording (medium-large)

**Status (2026-10-26): complete. `RecordResolveBatch`,
`RecordResolveStagingCopy` and `SubmitFrameTape` record the resolve dispatch,
its transitions and the staging-to-mirror copy; the mirror upload is an
`UploadBuffer` packet. Acceptance measured with the default-off
`fh1_d3d11_command_list_ab` diagnostic (staging `memcmp` + mirror
fingerprints): 4,000/4,000 rows equal over 22 extents, 199/199 bind checks ok,
trace-short md5 `b8f68e89c6192577747ab452aa5598ef` off and on. Two deviations
from this plan, both deliberate: the resolve split is
`RecordResolveMemory` (not `PrepareResolvePacket`/`ExecuteResolvePacket`), and
the tape is submitted at the resolve boundary, not at the swap — with draws and
the swap still immediate, a whole-frame hold would run the recorded resolve
after later draws. `IssueCopy` stays immediate; the transfers recorded are the
resolve's staging copy and the mirror upload. See
`DX12_PHASE3_M2_RESULT.md`.**

Deliver:
- `ResolveToMemory` split into `PrepareResolvePacket` (plan, one-use SRVs, constants,
  transitions) and `ExecuteResolvePacket` (bind, dispatch, unbind);
- `IssueCopy` appends transition/dispatch/UAV-barrier/copy packets instead of executing
  when recording;
- the staging→mirror copy uses `Copy` packets; the mirror upload path appends
  `UploadBuffer` packets (Phase 2 H42);
- T_frame is opened lazily and submitted at the swap boundary.

Accept (flag on vs off, same swap window, GTX 570):
- resolve staging `words_nz`/`dwords_nz`/`first` and the mirror before/after hashes match
  the immediate path bit-for-bit;
- the OM-unbind blanking never appears (`FH1 resolve bind check` srv0 = ok);
- draw counters unchanged; no unexpected transition logs;
- flag-off trace-short unchanged.

### M3 — Draw recording (large)

**Status (2026-10-26): complete, with two acceptance items reported as not
measurable as written. `RecordDrawPacket` captures shaders, the five constant
blobs (record-time bytes, uploaded at replay), SRVs/samplers, RTVs/DSV,
viewport/scissor, topology and the index buffer (ring page + offset, or the
shared-memory mirror); the texture cache appends `UploadTexture` packets; the
tape is drained before every diagnostic GPU read and at target prep. Measured:
trace-short on defaults md5 `b8f68e89c6192577747ab452aa5598ef`; 499,200 replayed
draws validated with **0 failures** (including viewport/scissor); 430,681 draw
packets with **0 draws left immediate**; replay's GPU constant block equals the
packet blob. Two defects were found and fixed on the way: a boxed
`UpdateSubresource` that did not land on this driver (the GPU kept zeros while
the packet held the right dwords) and a draw with no viewport that would have
replayed as a zero-sized one. Cross-run pixel identity ("dumps match", "no
visual difference") is not measurable for this title - the guest advances on
wall clock, and two runs of the *same* configuration differ by 3x in colour
count - so same-swap t5 metrics in the same Phase 12 family plus per-draw state
validation are reported instead. One ON run presented a fully black frame at
output frame 5400 while its front-buffer readback held content; not reproduced
on a second run, recorded for M4. See `DX12_PHASE3_M3_RESULT.md`.**

Deliver:
- `IssueDraw` split into `PrepareDrawPacket` and `ExecuteDrawPacket`; the packet captures
  shaders, five constant-buffer blobs, SRVs, samplers, RTVs/DSV, viewport/scissor,
  topology, index buffer and draw args;
- constant updates stop mapping a shared buffer at record time; replay uploads the
  packet's bytes before each draw;
- converted index ring pages are captured by buffer/offset (record-time writes are safe
  because pages are not reused within a frame);
- texture cache uploads append `UploadTexture` packets; the cache keeps its CPU untiling
  at record time but does not issue `UpdateSubresource` while recording;
- guest-output/P1 and paint/P2 remain immediate.

Accept:
- `fh1_debug_composite_dump` / frame dumps and the t5 dump metrics match the immediate
  path (60% `0x00007F7F`, 85% B==0, 17,889 RGB triples are the Phase 12 baseline; with
  Phase 13's flag state unchanged, the on/off comparison must be identical, not improved);
- draw counters identical (issued, drawn, dropped by reason);
- no draw with an unbound constant buffer or index buffer in the validation log;
- no visual difference in the free-roam captures when comparing on vs off.

### M4 — Swap list, readback and fences (medium)

Deliver:
- T_swap packet for the swap texture upload; `F_frame` wait before the readback;
- `F_swap` signal after P1; mailbox slot fence tracking; `F_present` for P2;
- `WaitForGpu` replaced by queue waits when recording, kept as-is when off;
- diagnostics that read GPU data mid-frame use `DrainForDiagnostic()` (the refresh split,
  Phase 2 H27).

Accept:
- `fh1_d3d11_resolve_readback=1` on/off: the front-buffer readback hash and guest bytes
  match the immediate path;
- `fh1_resolve_guest_refresh=1`: no stale texture load; the refresh split occurs only
  when expected;
- mailbox images are not overwritten before their `F_swap` completes;
- swap wait timing reported.

### M5 — State tracker enforcement across all handoffs (medium)

Deliver:
- emit transitions for every Phase 2 row H1–H42 at the packet builders;
- validation mode (`fh1_d3d11_command_list_validate=1`) checks each packet's required
  state against the tracker, logs mismatches with resource type and state names;
- explicit registration for swap-chain buffers and presenter images;
- deferred release of one-use views, evicted textures and tape slots.

Accept:
- a 90-second free-roam run with validation on reports zero unexpected transitions and
  zero unknown resources beyond the explicitly registered presenter/swap-chain set;
- the same run with validation off is visually and numerically identical to off-path
  captures;
- no resource leaked at shutdown (the existing COM teardown order).

### M6 — Performance and regression (medium)

Deliver:
- per-frame counters: packets, arena bytes, record CPU µs, replay CPU µs, submission
  count, `F_frame` wait µs, deferred-release count;
- a repeatable off/on A/B on the GTX 570, monitor 2 only;
- a written comparison against the free-roam gate's floors.

Accept:
- trace-short on defaults unchanged;
- flag-off performance identical within noise;
- flag-on numbers reported honestly; if the free-roam gate fails, the failure and the
  cause (GPU start delayed by recording) are documented as a later performance item,
  not hidden.

### M7 — Handoff (small)

Deliver:
- `DX11_FINISH_NOTES.md` entry with the off/on measurements and the exact commands;
- a one-page summary of what is wired, what is not (P2, native path), and the recommended
  next step;
- default remains off.

## 5. Test matrix

| Test | Flag | Expected |
|---|---|---|
| Build | off | `BUILD_EXIT=0`, no new warnings in the D3D11 TUs |
| Unit tests | — | `unit_tests "d3d11*"` all pass, including the new tape/tracker cases |
| Trace-short | off | md5 `b8f68e89c6192577747ab452aa5598ef` |
| Resolve A/B | off vs on | staging and mirror hashes identical; bind check `srv0=ok` |
| Draw A/B | off vs on | draw counters identical; frame/t5 dumps identical |
| Swap A/B | off vs on | front-buffer readback hash identical; no stale frame |
| Validation run | on + validate | zero unexpected transitions; zero unknown resources |
| Free-roam gate | off | unchanged pass/fail vs the current baseline |
| Free-roam gate | on | measured; pass/fail reported, not forced |
| Teardown | on | no leaked COM objects, no crash on shutdown |

Gate invocation (same profile as the Phase 13 run):

```
set REX_FH1_D3D11_SHADER_PACK=true
set REX_FH1_NATIVE_EXECUTOR=1
set REX_FH1_D3D11_COMMAND_LISTS=0   (then 1)
py -3.11 tools\run-fh1-render-test.py config\render-tests\fh1-free-roam.fh1test ^
  --state-root <same profile as the Phase 13 run> --output .local\diag\p3
```

Build: `.local\muse_build_game.bat`, then check `.local\muse_build_game.log` for
`BUILD_EXIT=0`. Unit tests: the `unit_tests` target from the same preset, filtered with
`"d3d11*"`. All runs on the GTX 570 by name, windows on monitor 2 only.

## 6. Performance questions to answer

1. How many packets and arena bytes does a free-roam frame produce? (Cap headroom.)
2. How much CPU time does packet preparation add per frame, and how much replay?
3. How much later does the GPU start versus the immediate path, measured as the gap
   between the first recorded packet and the T_frame submission?
4. Does the single frame fence cost more than the existing `WaitForGpu`?
5. Does the on path meet the free-roam 150 ms median / 5 Hz floor? If not, is early
   submission at the resolve boundary or streaming replay worth a later performance
   experiment?

These are measurements, not acceptance criteria for correctness. Phase 3 can be complete
and correct while slower; it cannot be complete while the numbers are unknown.

## 7. Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Sticky D3D11 state leaks between packets | Wrong draws, intermittent artifacts | Every packet rebinds everything; validation mode re-checks state |
| Constant buffers overwritten before replay | Every draw sees the last constants | Inline constant bytes in the packet; upload at replay |
| Mirror CPU upload reorders with recorded reads | Stale or future vertex/index data | `UploadBuffer` packets in T_frame (H42); never immediate while a tape is open |
| Texture upload executes before earlier recorded draws | Wrong sampled content | `UploadTexture` packets; cache defers the `UpdateSubresource` |
| One-use resolve views released before replay | Use-after-free / dropped binds | Packets hold COM refs; release only when the tape resets after its fence |
| Tape overflow | Out-of-order fallback | Submit at the overflow point and open a new tape; count and log |
| Event query reused while in flight | Corrupt wait | One queue mutex; wait before reusing a tape/query |
| P2 submits before P1 completes | Tearing/stale mailbox | P1 and T_swap share a critical section; publish after `F_swap`; mailbox slot fences |
| Recording latency fails the gate | Perf regression | Measure, report, keep default off; a later performance phase decides on early submission |
| Refactor of IssueDraw changes the off path | Regression on defaults | Off path keeps the existing execute code; A/B trace-short after every milestone |

## 8. Rollback

- Every milestone lands with the flag off by default.
- Reverting a milestone is deleting/undoing its files behind the flag; the off path does
  not depend on them.
- `.musebak-20261026` copies exist for every edited file before the first edit.
- No default is flipped in this phase; the final recommendation is Seth's decision.

## 9. What Phase 3 does not do

- Tape P1 or P2, or change the presenter/swap-chain ownership.
- Implement the native DX12 command list, barriers or fences; it only records the
  structures the native mapping will consume.
- Change Phase 13's resolve/alias behaviour or any of its flags.
- Touch the shader pack, DXC/DXIL, or the 29 missing boot shaders.
- Optimize the recording latency; that is measured here and addressed in a later
  performance phase.
- Modify `F:\PinyonShift\work` outside `.local\rexglue` and the notes.

## 10. Read-only verification (plan phase)

- The Phase 2 and Phase 3 plans were produced in one read-only pass; the check timestamp
  for the whole pass is 2026-10-26T19:04:28Z.
- `find` over `.local/rexglue/src`, `include`, `tools`, `config` with
  `-newermt @1793041468` → **0 files**.
- All Phase 3 planning operations were reads plus the creation of this file and the
  Phase 2 H42 addition under `F:\PinyonShift\dx12-phase0`. No build, no game run, no
  default change.

## 11. First action after approval

M0: snapshot the five files that will be touched first (`flags.h`, `flags.cpp`,
`command_processor.h`, `command_processor.cpp`, `src/graphics/CMakeLists.txt`), add the
flag and the empty components, build, and run the flag-off trace-short. No recording code
is wired until M1's unit tests pass.
