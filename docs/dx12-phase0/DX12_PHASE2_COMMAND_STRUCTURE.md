# DX12 Phase 2 — Command-List Granularity and Handoff Barriers

Status: Phase 2 only. Structure and handoff map. No translation code, no wrappers, no
default changes.
Generated: 2026-10-26 (UTC), from the Phase 1 slices and a read-only pass over the live
tree.
Inputs: `DX12_PHASE0_INVENTORY.md` (75 backend entry points / 394 sites) and
`DX12_PHASE1_SLICES.md` (seven slices, two seams, invariants I1–I11).
Output folder: `F:\PinyonShift\dx12-phase0`.

## 0. Decision

1. **Two recorded command lists per guest frame** (a list is a software tape on the D3D11
   compatibility path and an `ID3D12GraphicsCommandList` on the native path):
   - **T_frame** — all backend work: S3 surface setup, S7 draws with S6 state, S4
     resolves, S2/S4 transfers. Submitted once, at the pre-swap boundary.
   - **T_swap** — S5's swap-texture upload and its transition to a shader resource.
     Recorded only after the frame fence completes and, when the readback path is
     enabled, after the CPU readback has updated guest memory.
2. **Presentation is not taped in this phase.** The guest-output blit (P1) and the
   swap-chain paint/UI/present section (P2) are immediate-context sections owned by S1,
   ordered after T_swap on the same queue. On the native DX12 path P2 becomes a third
   list; the barrier map below already covers its transitions.
3. **One direct queue, serialized submission, one open tape per producer.** The guest
   thread records and submits T_frame/T_swap; the UI thread records and submits P2. A
   single submission mutex covers replay, fence signal and queue submission. Recording
   outside the mutex is allowed; execution order is submission order. P1 is issued inside
   the same critical section as T_swap's replay, before the mailbox publish, so the UI
   cannot submit P2 between them.
4. **One fence object, values per submitted list.** `F_frame` after T_frame, `F_swap`
   after T_swap and P1, `F_present` after P2. No per-resource fences. On the D3D11 path the fence
   is the existing event query; on the native path it is `ID3D12Fence` with the same
   values.
5. **Resource transitions are recorded, not worked around.** Every handoff below is a
   transition barrier, a UAV barrier, a fence wait, or submission order. The D3D11
   replay treats transitions as no-ops but must keep the OM unbind, because D3D11 still
   drops an SRV that is bound as a render target.
6. **No per-slice lists, no per-resolve lists, and no mid-frame split** except the
   refresh split (§4.5), which exists only when a texture load needs GPU-written bytes
   during the frame.

## 1. Constraints that force this structure

- **One guest command stream.** The backend has one device and one immediate context; no
  deferred contexts or command lists exist on the D3D11 path (Phase 0 §7b), so a "list"
  here is a recorded tape replayed in order, not concurrent recording.
- **A mandatory CPU readback sits between the resolve and the swap upload.**
  `IssueSwap` waits for the GPU, optionally reads the front-buffer extent back from the
  mirror, then `RequestSwapTexture` invalidates and untiles guest memory
  (`command_processor.cpp:3200-3264`, `texture_cache.cpp:769-771`). A single list per
  frame cannot contain both sides of a CPU wait; the split at that point is structural,
  not an optimization.
- **Presentation runs on a different cadence and thread.** `RefreshGuestOutput` blits
  into a mailbox image and requests a paint; `PaintAndPresentImpl` acquires the back
  buffer on the UI thread and presents
  (`presenter.cpp:502-554`, `d3d11_presenter.cpp:221-236`, `:328-365`, `:505`). The swap
  chain and its RTVs are UI-owned, so taping the paint would require a presenter
  abstraction that Phase 1 deliberately kept out of the backend boundary.
- **One frame fence.** Phase 1 S2.5 permits one fence object plus diagnostic waits. Two
  lists per frame produce two values for the frame's GPU work (the present section adds
  one more); per-slice lists would produce six or more waits with no parallelism to show
  for them.
- **The frame is serial at every handoff.** Draws → resolve → copy → readback → upload →
  paint. The in-tree D3D12 backend models the same serialization with one open deferred
  tape and submission splitting only when it helps
  (`include/rex/graphics/d3d12/command_processor.h:340-350`, `:479`), and treats a
  submission boundary as an implicit full UAV/aliasing barrier (`:347`).
- **This is a compatibility path.** Translation into D3D11 is for correctness and Fermi
  feature-level 11_0, not for throughput. The structure must therefore be simple and
  auditable before it is fast.

## 2. Alternatives considered

| Option | Verdict | Reason |
|---|---|---|
| One list per frame | Rejected | A CPU readback separates the resolve from the swap upload; merging would read stale guest bytes or force a blocking read inside a list. |
| One list per Phase 1 slice (7) | Rejected | S6 is state-only and has no commands of its own, while S3's clears belong inside the frame rather than in a separate submission; S2 and S4 share the staging→mirror copy boundary; six cross-list fences per frame violate S2.5 and buy no parallelism on one guest stream. |
| One list per resolve (~23 per swap) | Rejected | A fence per resolve, allocator/tape churn per resolve, and the Phase 12b probe already shows each extent resolved twice; the contract requires one frame fence, not one per resolve. |
| Per-pass lists inside the frame | Rejected for now | Scene/copy/present splits add submissions and fence points without starting GPU work earlier; the tape is recorded before replay, so splitting does not overlap recording with GPU execution. |
| Streaming replay (flush while recording) | Deferred | Would bound recording latency, but changes the submission model and needs state-restore validation. Later performance-phase work, not Phase 2 structure. |
| **Two lists plus an immediate present section** | **Chosen** | Matches the forced readback boundary, keeps one fence per list, leaves S1's UI-owned paint outside the recorder, and maps cleanly to three lists on native DX12. |

## 3. Lists, passes, and timeline

### 3.1 List composition

| List / section | Thread | Passes | Recorded when | Submitted when | Fence |
|---|---|---|---|---|---|
| **T_frame** | guest | A surface setup (S3), B scene (S7+S6), C resolves (S4), D transfers (S2/S4) | during the guest frame | at the pre-swap boundary, before the readback | `F_frame` |
| **T_swap** | guest | E swap-texture upload (S5) | after `F_frame` completes and, when enabled, the readback has updated guest memory | immediately after recording | `F_swap` (after P1) |
| **P1** guest-output blit | guest | guest-output image blit into the mailbox (S1) | after T_swap is submitted | immediately, before publishing | `F_swap` (signalled after P1) |
| **P2** paint/present | UI | acquire back buffer, clear, guest-output blit, UI drawers, Present (S1) | when the mailbox has an image | on the UI paint tick | `F_present` |

T_frame passes in order:

- **A. Surface setup (S3).** Creation clears for surfaces first used this frame; view and
  state registration. No draw may precede the clear of the surface it targets.
- **B. Scene (S7 + S6 + S3).** Guest draws, transfer passes, their binds, constant data
  and targets.
- **C. Resolves (S4).** One dispatch per planned resolve, source unbind, staging→mirror
  copies.
- **D. Transfers (S2 + S4).** CPU upload chunks, MSAA readback copies, diagnostics.

T_swap pass:

- **E. Swap upload (S5).** Untiled guest bytes → swap texture, then the texture's
  transition to a shader resource.

P1/P2 are presentation sections, not tapes in this phase. P1 writes a mailbox image; P2
reads it and presents.

### 3.2 Timeline

```
guest thread                                   UI thread
------------                                   ---------
open T_frame
  pass A: clears
  pass B: draws
  pass C: resolves
  pass D: transfers
submit T_frame  -> F_frame
wait F_frame
  (readback mirror -> guest, if enabled)
record T_swap
submit T_swap
  P1: blit mailbox image   (same critical section)
signal F_swap
publish mailbox ------------------------------> wait for image
                                               open P2 (immediate)
                                                 acquire back buffer
                                                 clear, blit, UI
                                                 transition to PRESENT
                                                 execute; signal F_present
                                                 Present
                                               close P2
```

If T_frame has no commands at all (no clears, draws, resolves or transfers), it is not
submitted and no fence value is consumed; the wait is skipped. If T_swap has no upload (no resolvable
front buffer), it is not submitted and P1 publishes the black output instead.

## 4. Fence policy

### 4.1 One fence object

- `fence_value_` starts at 0. Each submitted list or section gets the next value.
- `F_frame` = value after T_frame; `F_swap` = value after T_swap and P1; `F_present` =
  value after P2's commands and before `Present`.
- The single fence is the only synchronization object. Diagnostics may wait on it too,
  but may not create a second fence.

### 4.2 Wait sites

| Wait | Before | Why |
|---|---|---|
| `F_frame` | the swap readback and T_swap recording | the mirror's resolve output is about to be read on the CPU |
| `F_frame` | a mid-frame texture refresh (S5.3) | the guest range's GPU-written pages must be read back before the CPU untiles |
| the `F_swap` value recorded when a mailbox slot was last written | rewriting that mailbox image | the image may still be sampled by P2 or P1 of a previous frame |
| the last `F_present` that used a back-buffer index | reusing that swap-chain buffer (native mapping) | P2 may still be reading/writing it; the D3D11 swap chain provides this on the compatibility path |
| `F_frame`, `F_swap`, `F_present` | resize, cache clear, teardown | no resource may be released or resized while a submitted list references it |
| fence of the referencing list | reusing a tape/allocator slot, a constant-buffer ring slot, or a deferred-freed texture/view | the GPU may still read the recorded resource |

### 4.3 D3D11 realization

The compatibility path implements signal/wait with the existing `D3D11_QUERY_EVENT`
(`command_processor.cpp:3113-3130`): `End` after the last command of the list, spin on
`GetData` for the wait. The query reports completion of all earlier queue work, which is
equivalent to `F_frame` because the queue is serial and every earlier list is complete by
then. On the native path the same values become `ID3D12Fence::Signal`/`SetEventOnCompletion`.

The submission mutex covers `End`, `GetData`, replay, submission and P1's immediate
commands. It is released before `Present` blocks, but no submission that targets the same
back-buffer index may start until `F_present` for that index has completed.

### 4.4 Cross-list visibility

- A completed fence is a full memory barrier for CPU-visible data. Page-valid and
  GPU-written state may only change after the corresponding wait.
- GPU-side UAV/aliasing visibility across lists is covered on the native path by the
  documented submission-boundary semantics; on the compatibility path the same-context
  execution order and the explicit unbinds cover it. No extra global barrier is recorded
  at list boundaries.

### 4.5 The only allowed mid-frame split

If a texture load (S5.3) needs a range the GPU wrote, and T_frame is still open:

1. close and submit T_frame, signal `F_frame`;
2. wait `F_frame`, read the range back from the mirror to guest memory;
3. open a new T_frame for the remaining commands and record the texture upload there.

This split exists only when `fh1_resolve_guest_refresh` (or another refresh path) is
enabled. With the defaults (both refresh flags off) there is exactly one T_frame per
frame. A split does not reset the resource-state tracker; it only adds a fence value.

CPU mirror uploads are **not** a split trigger: they are recorded as upload packets in
T_frame (H42), with the bytes copied into the tape, and replayed in order.

## 5. Resource-state policy

### 5.1 State sets

| Resource | Initial | States used | Granularity |
|---|---|---|---|
| EDRAM colour surface | `RENDER_TARGET` | `RENDER_TARGET`, `NON_PIXEL_SHADER_RESOURCE` | whole resource (single subresource) |
| EDRAM depth surface | `DEPTH_WRITE` | `DEPTH_WRITE`, `NON_PIXEL_SHADER_RESOURCE` | whole resource; DSV/SRV sharing |
| Guest-memory mirror | `COMMON` | `MIRROR_READ` = `INDEX_BUFFER` + `NON_PIXEL_SHADER_RESOURCE` + `VERTEX_AND_CONSTANT_BUFFER`, `COPY_DEST` | whole buffer, never per page |
| Resolve staging | `COPY_DEST` | `UNORDERED_ACCESS`, `COPY_SOURCE`, `COPY_DEST` | whole buffer |
| Readback buffer | `COPY_DEST` | `COPY_DEST`, then mapped | whole buffer (readback heap) |
| Texture cache texture | `COMMON` | `COPY_DEST`, `PIXEL_SHADER_RESOURCE` / `NON_PIXEL_SHADER_RESOURCE` | per mip/slice |
| Swap texture | `COPY_DEST` | `COPY_DEST`, `PIXEL_SHADER_RESOURCE` | whole texture |
| Guest-output mailbox image | `RENDER_TARGET` | `RENDER_TARGET`, `PIXEL_SHADER_RESOURCE` | whole texture, per mailbox slot |
| Swap-chain back buffer | `RENDER_TARGET` | `RENDER_TARGET`, `PRESENT` | per buffer index |
| Constant-buffer ring | upload heap, `GENERIC_READ` | none (CPU-written) | per slot |
| Converted vertex/index buffers | `COPY_DEST` | `COPY_DEST`, `VERTEX_AND_CONSTANT_BUFFER` / `INDEX_BUFFER` | whole buffer |
| UI intermediate images (P2) | `RENDER_TARGET` | `RENDER_TARGET`, `PIXEL_SHADER_RESOURCE` | per effect pass |

`MIRROR_READ` is a read-only combination; the mirror is never in
`UNORDERED_ACCESS` on the production path (the direct-mirror UAV is diagnostic and
outside this contract).

### 5.2 Rules

1. **Explicit initial states.** Every resource is registered in the tracker at creation
   with the state in the table. The tracker persists across lists and frames; a list or
   frame boundary never resets a resource's state. The recorder never relies on implicit
   promotion or decay.
2. **Transition only on a change.** If the tracked state equals the requested state, no
   barrier is recorded (the native backend's `PushTransitionBarrier` already returns
   false for equal states, `d3d12/command_processor.cpp:270-284`).
3. **UAV barrier before leaving UAV.** A resource written by a dispatch gets a UAV
   barrier before it is transitioned to `COPY_SOURCE` or read. In the resolve path this is
   the staging buffer between `Dispatch` and the copy.
4. **Barriers are batched per pass** and recorded in the tape. The D3D11 replay ignores
   transitions; it does not ignore the OM unbind.
5. **OM unbind is separate.** The resolve transition makes the surface readable; the
   render-target view must also be removed from the OM set. Both are recorded, in that
   order, before the source SRV is bound.
6. **No per-page mirror states.** The 512 MiB mirror is one buffer with one tracked
   state. Page-valid/GPU-written are CPU ownership bits, not D3D12 states.
7. **No aliasing barriers.** Host resources are distinct allocations. Guest-address
   aliasing (two resolves into one base) is a writer contract, not a D3D12 resource
   alias; it is outside this map and remains Phase 13's concern.
8. **No split barriers.** A transition is a full barrier; there is no begin/end pair.
9. **Tracker ownership.** The command-recording core that owns the stream (S1's stream
   side) owns the state table. Slices request a state through the recorder; they never
   emit a transition from inside a cache or a surface. This is the piece Phase 1 left
   open (S1 vs S3/S5).

## 6. Handoff map

Every Phase 1 ordering rule is listed. `None (CPU)` means the rule is enforced by the
recorder's single-threaded order and needs no GPU primitive.

### 6.1 Inside T_frame

| ID | Phase 1 rule | Handoff | GPU primitive | D3D11 realization |
|---|---|---|---|---|
| H1 | S1.2 | All frame commands | One queue; list order = execution order; no parallel recording | Tape replay to the immediate context |
| H2 | S3.1, S3.2 | Surface creation → first use | Created in `RENDER_TARGET` / `DEPTH_WRITE`; clear recorded before the first draw; no transition on creation | `ClearRenderTargetView` / `ClearDepthStencilView` |
| H3 | S6.1, S6.2 | Shader/state creation → bind | None (objects must be resident before the list executes) | Pipeline cache |
| H4 | S6.3 | Constant update → draw/dispatch | No barrier; upload-ring write ordered before submission; slot fenced before reuse | `Map(WRITE_DISCARD)` / `UpdateSubresource` |
| H5 | S7.1, S7.6 | Draw decode, topology | None (CPU) | Primitive processor / topology |
| H6 | S7.2 | Guest-DMA index fetch | Mirror: `COMMON`/`COPY_DEST` → `MIRROR_READ` | Bind mirror SRV/index buffer |
| H7 | S7.3 | Bind sequence | None (descriptor/state order) | Bind calls |
| H8 | S7.4, S7.5 | Draw into a surface | Surface `NON_PIXEL_SHADER_RESOURCE` → `RENDER_TARGET` / `DEPTH_WRITE` if it was resolved since its last draw (otherwise already a target); OM set | `OMSetRenderTargets` |
| H9 | S3.4 | Target XOR SRV | No combined write/read state; the resolve transition enforces it | OM unbind |
| H10 | S4.3 | Resolve source read | Surface `RENDER_TARGET` / `DEPTH_WRITE` → `NON_PIXEL_SHADER_RESOURCE`; RTV/DSV removed from OM | `OMSetRenderTargets(null)` + no-op transition |
| H11 | S4.4 | Bind resolve pipeline → dispatch | Descriptors after the transition; `Dispatch` | `CSSet*`, `Dispatch` |
| H12 | S4.5 | Dispatch → unbind/release | UAV barrier on the staging buffer; deferred release until `F_frame` | `CSSetUnorderedAccessViews(null)` |
| H13 | S4.6, S2.2 | Resolve staging → mirror | Staging `UNORDERED_ACCESS` → `COPY_SOURCE` (UAV barrier first); mirror `COPY_DEST`; copy | Unbind UAV + `CopySubresourceRegion` |
| H14 | S2.1 | CPU upload chunk | Mirror `COPY_DEST`; upload staging `COPY_SOURCE`; copy | `MakeRangeValid` + copy |
| H15 | S4.8 | MSAA readback | MSAA `RENDER_TARGET` → `RESOLVE_SOURCE`; dest `COPY_DEST` → `RESOLVE_DEST` → `COPY_SOURCE`; copy | `ResolveSubresource` + `CopyResource` |
| H16 | S3.5 | Depth resolve source | Depth resource `DEPTH_WRITE` → `NON_PIXEL_SHADER_RESOURCE` | DSV/SRV bind |
| H17 | S4.7 | Diagnostic readback | A mid-frame diagnostic that must be read on the CPU uses the same split rule as H27, or is deferred to the frame boundary; it never reorders production copies | `WaitForGpu` + map |
| H18 | S3.3, S3.6 | View/format creation | None (CPU) | Host render config |
| H19 | S6.4, S6.5 | Bind order, GS/IA | None (CPU) | Bind sequence |
| H20 | S4.1, S4.2 | Plan resolve, choose owner | None (CPU) | `Fh1PlanResolve`, owner selection |
| H21 | S5.1, S5.2 | Key, create vs load | None (CPU) | Texture cache lookup |
| H22 | S5.4 | Texture upload → draw | Texture `COPY_DEST` → shader-resource state before bind | `UpdateSubresource` + SRV |
| H23 | S5.7 | Sampler creation | None (CPU) | Sampler cache |
| H42 | S2.1 | CPU mirror upload → recorded reads | Upload packet ordered before the packets that read the mirror; mirror `COPY_DEST` → `MIRROR_READ` at the next read; no fence | Replay maps the staging buffer, copies, then reads |

### 6.2 T_frame → T_swap

| ID | Phase 1 rule | Handoff | GPU primitive | D3D11 realization |
|---|---|---|---|---|
| H24 | S2.3, S4.9 | Resolve writes → CPU read | Signal `F_frame`; CPU wait; mirror → `COPY_SOURCE`; copy to readback buffer (`COPY_DEST`); map | Event query `End`/`GetData`; staging readback |
| H25 | S2.4 | GPU-written pages → CPU upload | The fence makes the writes visible; readback-to-guest clears GPU-written; uploads skip GPU-written pages | `CopyReadbackToGuest` |
| H26 | S5.5 | Front-buffer readback → swap upload | After the readback: invalidate CPU-valid, untile, transition the swap texture from its previous shader-resource state to `COPY_DEST`, upload, transition to shader resource | `RequestSwapTexture` / `LoadTextureData` |
| H27 | S5.3 | Mid-frame texture refresh | Close T_frame, `F_frame`, wait, readback; open a new T_frame; upload recorded there | `RefreshResolvedGuestRange` (flag-gated) |
| H28 | S2.5 | Fence identity | One fence object; `F_frame`, `F_swap` and `F_present` values only | One event query |
| H29 | S4.5, S5.6, S6.3 | Deferred release / slot reuse | Wait the fence of the list that referenced the resource or slot | Deferred free / cache eviction |
| H30 | S1.5 | Resize/teardown | Wait `F_frame` + `F_swap` + `F_present` | `WaitForGpu` before release/resize |

### 6.3 T_swap, P1, P2

| ID | Phase 1 rule | Handoff | GPU primitive | D3D11 realization |
|---|---|---|---|---|
| H31 | S5.5 | Swap texture → SRV | Texture `COPY_DEST` → `PIXEL_SHADER_RESOURCE`; the next T_swap transitions it back to `COPY_DEST` | SRV bind |
| H32 | S1.4 | Upload → guest-output blit | Queue order; source SRV readable; mailbox image `PIXEL_SHADER_RESOURCE` → `RENDER_TARGET` before the blit | `OMSetRenderTargets` + `Draw(4)` |
| H33 | S1.4 | Blit → publish | P1 submitted before the mailbox publish; `F_swap` signals after P1 | Publish deferrer / immediate replay |
| H34 | S1.4 | Mailbox slot reuse | Wait the `F_swap` value of that slot's last write before rewriting the image | Per-slot fence value |
| H35 | S1.4 | Mailbox image → paint | Image `RENDER_TARGET` → `PIXEL_SHADER_RESOURCE`; back buffer `RENDER_TARGET`; intermediates `PIXEL_SHADER_RESOURCE` ↔ `RENDER_TARGET`; the next P1 transitions the image back to `RENDER_TARGET` | Paint path binds |
| H36 | S1.4 | Paint → Present | Back buffer `RENDER_TARGET` → `PRESENT`; execute; signal `F_present`; `Present` | `Present` |
| H37 | S1.4 | Back-buffer reuse | Wait the last `F_present` for that index; then `PRESENT` → `RENDER_TARGET` on acquire | D3D11 swap chain; native per-index fence value |
| H38 | S1.7 | Thread ownership | Present section on the UI thread; vblank thread issues no device calls | Current presenter |
| H39 | S1.6 | Device loss | Stop submissions; report once through the GPU-loss callback | `Present` / `GetDeviceRemovedReason` |
| H40 | U1 | Vblank cadence | None (CPU/thread) | `WaitForVBlank` |
| H41 | P1 (translator) | DXBC production | None (CPU) | Runtime translator |

CPU-only rules with no GPU primitive: S1.1 (device before resources), S1.3 (swap-chain
tier), S3.1/S3.3 (key, view creation), S4.1/S4.2 (plan, owner), S5.1/S5.2/S5.7 (key,
create/load, sampler), S6.1/S6.2/S6.4/S6.5 (translate, create, bind order, GS/IA),
S7.1/S7.3/S7.6 (decode, bind order, topology), P1. They are enforced by the recorder's
order; none may be reordered around a fence without revisiting this table.

## 7. Failure and edge cases

- **Fence wait failure / device removed.** Stop submitting; report once; do not release
  GPU resources until the final drain either completes or the device is known lost. A
  failed wait never promotes pending work to complete.
- **Transition conflict.** If the tracker's state disagrees with a requested state, emit
  the transition from the tracked state and log once; never skip a required transition
  because the D3D11 path would tolerate it.
- **Readback failure.** The pages stay GPU-written; the swap upload is skipped and S5
  keeps its previous host content. Guest memory is not partially overwritten.
- **Empty lists.** No submission, no fence value. The timeline continues with the next
  non-empty list; waits use the last completed value.
- **Resize.** Wait all three values, release back-buffer RTVs and any references, resize,
  recreate, and re-register the new buffers as `RENDER_TARGET`.
- **Eviction while in flight.** A texture evicted after being recorded in a submitted list
  is deferred-freed until that list's fence, not released at eviction time.
- **Mailbox backpressure.** If the UI has not consumed a mailbox image, the next P1 waits
  on that slot's `F_swap`; the guest thread never blocks on the paint itself.

## 8. What Phase 2 settles, and what remains

Settled:
- Two recorded lists per guest frame plus an immediate present section; list contents,
  thread ownership and submission order.
- The forced split at the readback boundary and the only allowed mid-frame split
  (refresh).
- Fence identity, values, wait sites, and the D3D11 event-query realization.
- Resource state sets and the transition rules, including the OM-unbind-plus-transition
  pair at the resolve.
- Ownership of the state tracker by the recording core.
- A complete mapping of every Phase 1 ordering rule to a primitive.

Remaining for Phase 3 (implementation), not decided here:
1. The recorder API and tape command encoding; whether the fake path reuses the in-tree
   `DeferredCommandList` shape or a smaller D3D11-specific tape.
2. Whether P2 becomes a taped list immediately or stays an immediate section until a
   native DX12 presenter exists.
3. The exact tape/allocator slot count (three frame slots matching the three-buffer swap
   chain is the natural default) and tape size caps.
4. Barrier batching heuristics beyond "per pass".
5. Performance validation of recording latency (the tape is replayed after recording, so
   the GPU starts later than in the current immediate path). This is a later
   performance-phase measurement, not a reason to change the structure now.
6. The default-off selector for the DX12-style path; flipping any default remains Seth's
   call.

## 9. Out of scope

- Any code: recorder, tape, tracker, fences, wrappers. This phase is a structure and
  barrier map only.
- Phase 1's out-of-scope list carries forward: D3D11 features never called (Phase 0 §7),
  the D3D12/Vulkan backends, the DXBC translator and shader-pack track, Phase 13's
  shared-base work, tests and SDL's own renderer.
- Per-resolve fences, sparse/tiled resources, aliasing barriers for guest-address
  aliasing, and implicit state promotion/decay.
- Default changes. `fh1_native_executor`, `fh1_d3d11_resolve_readback` and
  `fh1_resolve_guest_refresh` stay as they are.

## 10. Read-only verification

- Start of Phase 2 work: 2026-10-26T19:04:28Z.
- Check run after writing this file with `find` over `.local/rexglue/src`, `include`,
  `tools` and `config` (`-newermt "@1793041468"`) → **0 files**.
- All Phase 2 operations were reads plus the creation of this file under
  `F:\PinyonShift\dx12-phase0`. No build, no game run, no default change, no write under
  `F:\PinyonShift\work`.
