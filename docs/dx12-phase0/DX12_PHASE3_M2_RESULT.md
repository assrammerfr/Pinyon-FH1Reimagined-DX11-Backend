# Phase 3 M2 — Resolve and transfer recording, measured against the immediate path

Date: 2026-10-26
Tree: `F:\PinyonShift\work\.local\rexglue` (live DX11 tree; no git; 9 of the 11
edited files snapshotted to `.musebak-20261026-m2` — see Backups at the end)
Flags, all default **false/0**:
`fh1_d3d11_command_lists` (the recorder), `fh1_d3d11_command_list_validate`
(unused), `fh1_d3d11_command_list_ab` (the measurement diagnostic added in M2).
Final binary: 2026-10-26 18:09, `out/build/win-amd64-release/pinyon_shift.exe`.
Device/driver: GTX 570 (feature level 11_0), monitor 2, 1280x720.

## Delivered

### The recorded resolve

- `D3D11CommandProcessor::RecordResolveBatch` — `command_processor.cpp:2566`.
  Appends the shared-memory unbind (`D3D11UnbindGraphicsSrvPacket`), the source
  SRV and destination UAV transitions, the resolve dispatch
  (`D3D11Fh1NativeResolve::RecordResolveMemory`, `fh1_native_resolve.cpp:393`,
  which also appends the resolve's constant-buffer payload into the arena) and
  the UAV barrier. A named resolution skip truncates the partial batch so the
  tape only describes what actually runs; a tape overflow submits at the
  overflow point and continues in a new tape (never measured in these runs).
- `D3D11CommandProcessor::RecordResolveStagingCopy` —
  `command_processor.cpp:2635`. The staging-to-mirror copy as a
  `D3D11CopyBufferPacket` (source offset 0, destination offset = the guest
  extent start, length = the extent), preceded by the two transitions. The
  replay sink implements it as the same `CopySubresourceRegion` box the
  immediate `CopyResolveStagingToHost` issues.
- `D3D11CommandProcessor::SubmitFrameTape` — `command_processor.cpp:2671`.
  Replays the open tape through the immediate-context sink and resets it.
  Submission is **at the resolve boundary** — `"resolve dispatch"` (line 3041)
  and `"resolve staging copy"` (line 3227) — not at the swap, because draws and
  the swap are still immediate in M2 and a whole-frame hold would run the
  recorded resolve after the draws that already ordered against it. Safety
  flushes remain before a draw (line 1362), before the resolved-range refresh
  (line 3371) and before the swap (line 3425).
- The mirror upload path records the upload: `AppendUploadBufferPacket` is
  called from the shared-memory upload device
  (`shared_memory_upload_device.cpp:99`); the 3,768,320-byte input upload of a
  1280x720 resolve appears as the arena payload of its batch (see the packet
  accounting below).
- `IssueCopy` is **not** re-routed in M2; the transfer path recorded here is the
  resolve's staging-to-mirror copy and the mirror upload. (M3/M4 own draws, the
  texture-cache uploads and the swap list.)

### The measurement diagnostic (default off)

`fh1_d3d11_command_list_ab` (`flags.cpp:59`) makes every recorded resolve run
**twice in one session, on the same source and the same plan**:

1. the recorded batch (dispatch + staging copy packet) through the tape, then
2. the immediate path (`ResolveToMemory` + `CopyResolveStagingToHost`).

Both staging buffers and both mirror extents are compared: staging by
`memcmp` over the whole extent (which subsumes `words_nz`, `dwords_nz` and
`first`), the mirror by the same 16-byte-stride FNV fingerprint the store probe
uses (`mirror_hash`, taken with a GPU wait). The log line is
`FH1 command list A/B: call=… len=… tape_read=… immediate_ok=… immediate_read=…
tape_words_nz=… immediate_words_nz=… tape_first=… immediate_first=…
equal=… tape_mirror=…/… immediate_mirror=…/… mirror_equal=…`
(`command_processor.cpp:3206`). The flag doubles the resolve GPU work; it is a
diagnostic, not a path.

### Why the equivalence is measured in-session

The render-test scenario is wall-clock driven and the two processes do not take
the same route: two flag-off runs of the same scenario already diverge in the
resolve stream (`.local/diag/p3m2-probe-off` vs `-off2`: the first 30 sampled
resolves match modulo guest addresses, then one run reports a `c2` resolve where
the other reports `c0`), and their presented-frame counts differ by 2x. Hash
comparison *between* runs therefore cannot prove anything; the A/B diagnostic
compares the two paths inside one process, at the same resolve, and is the
measurement M2 is accepted on.

## Verification (final binary, 18:09)

### Build and unit tests

- `.local\muse_build_game.bat` → **BUILD_EXIT=0** (15 steps, relink at 18:09).
- `unit_tests` rebuilt; `d3d11*` filter: **All tests passed (171 assertions in
  20 test cases)**; full suite: **346 test cases — 342 passed, 4 skipped,
  0 failed; 3,792 assertions passed**.

### Gate: trace-short, defaults vs flag on

Scenario `.local/probes/trace-short.fh1test`, seed `.local/diag/p3m0-off.state`
(`gpu_backend = "d3d11"`, `d3d11_adapter = "GTX 570"`), no env cvars for the off
run and only `REX_FH1_D3D11_COMMAND_LISTS=true` for the on run:

| run | result | RUN_EXIT | t0300.ppm | t0600.ppm |
|---|---|---|---|---|
| `.local/diag/p3m2f-off` (defaults) | pass | 0 | `b8f68e89c6192577747ab452aa5598ef` | `b8f68e89c6192577747ab452aa5598ef` |
| `.local/diag/p3m2f-on` (flag on) | pass | 0 | `b8f68e89c6192577747ab452aa5598ef` | `b8f68e89c6192577747ab452aa5598ef` |

Both captures are byte-identical to the recorded baseline in both flag states,
so the off path is unchanged and the on path does not alter a presented frame.

### The A/B equivalence (worldcomp swap window)

Scenario `.local/probes/worldcomp.fh1test` (captures at input frames 3500/3700,
stop 3800), seed `p3m0-off.state`, env `REX_FH1_D3D11_SHADER_PACK=true
REX_FH1_NATIVE_EXECUTOR=1 REX_FH1_SWAP_FETCH_TRACE=true
REX_FH1_D3D11_COMMAND_LISTS=true REX_FH1_D3D11_COMMAND_LIST_AB=true
--no-performance-gate`. Two runs: `.local/diag/p3m2f-ab` (store probe from 315)
and `.local/diag/p3m2f-ab2` (from 100).

- **4,000 A/B rows per run, 22 distinct extents, zero failing rows.** Every row
  has `tape_read=1 immediate_ok=1 immediate_read=1 equal=1 mirror_equal=1`; no
  row has `equal=0` or `mirror_equal=0`. Per-length table (p3m2f-ab2):

| extent (B) | rows | equal | mirror_equal |
|---|---|---|---|
| 4,096 | 942 | 942 | 942 |
| 8,192 | 390 | 390 | 390 |
| 16,384 | 248 | 248 | 248 |
| 24,576 | 260 | 260 | 260 |
| 36,864 | 130 | 130 | 130 |
| 49,152 | 5 | 5 | 5 |
| 61,440 | 260 | 260 | 260 |
| 65,536 | 71 | 71 | 71 |
| 122,880 | 130 | 130 | 130 |
| 163,840 | 5 | 5 | 5 |
| 196,608 | 3 | 3 | 3 |
| 245,760 | 423 | 423 | 423 |
| 262,144 | 78 | 78 | 78 |
| 589,824 | 5 | 5 | 5 |
| 983,040 | 68 | 68 | 68 |
| 1,146,880 | 247 | 247 | 247 |
| 1,310,720 | 464 | 464 | 464 |
| 2,031,616 | 6 | 6 | 6 |
| 2,166,784 | 6 | 6 | 6 |
| 2,621,440 | 15 | 15 | 15 |
| 3,768,320 | 241 | 241 | 241 |
| 4,194,304 | 3 | 3 | 3 |

  (`p3m2f-ab` has the same 22 extents with the same all-equal result, different
  row counts: 996/369/266/246/123/5/246/84/123/5/5/417/90/5/64/233/440/8/16/13/238/8.)
- Extents that matter for this project are in the set and equal: the 640x360
  c0 copy (`983,040`), the c3 downscale extent (`245,760`), the 520x520 depth
  resolve (`2,166,784`), and the 1280x720 resolves (`3,768,320`, `4,194,304`).
- Representative rows (p3m2f-ab2):

```
call=0    len=3768320 words_nz 921600/921600 first C6F16C6F/C6F16C6F
          tape_mirror B575141708FD60EC/230400 immediate_mirror B575141708FD60EC/230400 mirror_equal=1
call=13   len=245760  words_nz 0/0         first 00000000/00000000
          tape_mirror 4F9197D17ADBD383/0     immediate_mirror 4F9197D17ADBD383/0     mirror_equal=1
call=15   len=4096    words_nz 0/0         first 00000000/00000000
          tape_mirror DCE53C1DF8560F83/0     immediate_mirror DCE53C1DF8560F83/0     mirror_equal=1
call=2305 len=983040  words_nz 230400/230400 first 00007F7F/00007F7F
          tape_mirror 9E59FB948ACE03FC/1     immediate_mirror 9E59FB948ACE03FC/1     mirror_equal=1
call=3770 len=2166784 words_nz 33685/33685 first 00000000/00000000
          tape_mirror 2C88CF513AF3CFC2/8383 immediate_mirror 2C88CF513AF3CFC2/8383 mirror_equal=1
```

- Store probe (mirror before/after hashes around each probed resolve) on the
  final binary: off reference `.local/diag/p3m2f-offwc` 160/160 `ok=true`
  (129 `changed=true`); recorded path `.local/diag/p3m2f-ab2` 160/160 `ok=true`
  (36 `changed=true`); bind run 160/160 `ok=true` (30 `changed=true`). All
  hashes valid; the changed counts are not comparable across runs (different
  sessions and, in the A/B run, the diagnostic's own copies have already landed
  the extent when the probe reads it).

### Bind check (the OM/CSS unbind that guards the resolve blanking)

Run `.local/diag/p3m2f-bind`: `REX_FH1_DEBUG_RESOLVE_BIND_CHECK=true` with the
recorder on. **199 bind-check lines, 199 `srv0=ok srv1=ok uav=ok cb=ok`, zero
non-ok lines**; 0 WARN/ERROR lines in the whole run. The recorded batch's leading
unbind reproduces the immediate path's binding state on the replay.

### Tape accounting (bind run; the submit log is capped at 24 lines)

```
Counter({'resolve dispatch': 12, 'resolve staging copy': 12})
packets per submission: {3: 12, 5: 9, 6: 3}
arena bytes:             {0: 12, 32: 9, 3768352: 3}
```

- A dispatch batch is 5 packets (unbind, 2 transitions, dispatch, UAV barrier)
  or 6 when the mirror upload is inside it (arena 3,768,352 B — the 1280x720
  input upload); a batch with no upload carries only the 32-byte resolve
  constant payload.
- A staging copy is 3 packets (2 transitions + the buffer copy), 0 arena bytes.

### Captures and counters

- Every worldcomp run above (offwc, ab, ab2, bind) passes and both captures are
  `b8f68e89c6192577747ab452aa5598ef` — the recorded baseline, identical between
  flag off and flag on.
- The draw path is not recorded in M2: the only draw-path change is the no-op
  tape flush before a draw (`SubmitFrameTape("draw")` returns immediately on an
  empty tape when recording is on, and is a single cached-bool check when off).
  The draw-drop counters in the on runs are all zero
  (`dropped[vshader_unbound=0 fspin_unbound=0 both_shaders_unbound=0
  coherent_dst_in_copy_mode=0 screen_flipped_to_d24s8=0 …] other=0`).
- A cross-process draw-counter comparison is **not** a valid measurement here:
  the runs diverge (p3m2x2-off reached 1,402 presented frames / 40,304 draw
  calls; p3m2x2-bind reached 654 frames / 669,593 draw calls in the same wall
  clock), for the same reason the two flag-off staging traces diverge. The
  equivalence evidence is the in-session A/B above.

## Backups (no git in this tree)

Nested edits this pass are covered by `.musebak-20261026-m2` snapshots taken at
16:09 (the pass's starting state) in `.local\rexglue`: `command_list.h`,
`command_processor.h`, `fh1_native_resolve.h`, `shared_memory_upload_device.h`,
`command_list.cpp`, `command_list_immediate_sink.cpp`, `command_processor.cpp`,
`fh1_native_resolve.cpp`, `shared_memory_upload_device.cpp` — 9 files.
Two files changed without a fresh M2 snapshot: `include\rex\graphics\flags.h`
(the A/B cvar declaration, 16:45) and `src\graphics\flags.cpp` (the A/B cvar
definition and its description, 18:07); their nearest backups are the
`-20261026` ones, and the M2 deltas are the one declaration line and the
`fh1_d3d11_command_list_ab` definition block plus the description wording
quoted under Not in M2 / Delivered above.

## Defaults unchanged

`fh1_d3d11_command_lists` false (`flags.cpp:48`),
`fh1_d3d11_command_list_validate` false (`flags.cpp:54`),
`fh1_d3d11_command_list_ab` false (`flags.cpp:59`),
`fh1_d3d11_resolve_readback` false (`flags.cpp:39`),
`fh1_resolve_guest_refresh` false (`flags.cpp:272`),
`fh1_native_executor` 0 (`flags.cpp:283`). Nothing was flipped for these
measurements; the on-state runs set only the cvars named in each command line.

## Not in M2

- Draws, the texture-cache upload path, the swap list and the readback path are
  still immediate; only the resolve and its mirror transfer are recorded.
- `D3D11SubmissionQueue`/fences are still the M0 placeholder — submission uses
  the immediate context under the resolve boundary, so there is no
  frame-granular list to wait on yet.
- `fh1_d3d11_command_list_validate` is still unused; transition legality is
  only what the state map enforces.

## M3 handoff

1. Split `IssueDraw` into prepare/execute and record the draw packet (shaders,
   five constant blobs, SRVs, samplers, RTV/DSV, viewport/scissor, topology,
   index buffer, draw args), uploading constant bytes at replay instead of
   mapping a shared buffer at record time.
2. Route texture-cache uploads as `UploadTexture` packets and stop issuing
   `UpdateSubresource` while recording.
3. With draws on the tape, move submission to the frame boundary and open the
   second (swap) list; keep `fh1_d3d11_command_list_ab` off — it doubles the
   resolve work by design.
