# Phase 3 M1 — Tape and tracker core with device-free tests

Date: 2026-10-26
Tree: `F:\PinyonShift\work\.local\rexglue` (live DX11 tree; no git)
Flag: `fh1_d3d11_command_lists` default **false**. No runtime pass is routed
through the tape in M1; the immediate path remains the reference.

## Delivered

### Packet stream and arena

`include/rex/graphics/d3d11/command_list.h`,
`src/graphics/d3d11/command_list.cpp`

- `D3D11Command` is a `std::variant` of the Phase 2 packet types:
  barrier, UAV barrier, unbind-RT, dispatch, buffer copy, whole-resource copy,
  subresource copy, buffer upload, texture upload, clear RTV, clear DSV,
  resolve.
- Every packet owns its COM references (`Microsoft::WRL::ComPtr`), so a tape
  keeps its resources alive independently of the cache that recorded them.
- Arena: append-only byte vector, 16-byte alignment, spans are
  `{offset,size}` from the tape base. Default caps are 32,768 packets and
  64 MiB; both are constructor parameters so tests can force the caps.
  `Append`/`AppendBytes` set a sticky `overflowed()` and return null/false at a
  cap; `Reset()` clears packets, arena contents and the overflow flag while
  keeping capacity.
- `Replay(sink)` walks packets in order and passes the tape alongside each
  packet so arena-backed payloads can be resolved with `Data(span)`. Invalid
  or empty spans resolve to null.
- `D3D11CommandSink::Execute(command, tape)` is the replay interface;
  `CreateD3D11ImmediateCommandSink(context)` is the D3D11 implementation in
  `command_list_immediate_sink.cpp`.

### Immediate-context replay sink

`src/graphics/d3d11/command_list_immediate_sink.cpp`

- Dispatch: optional OM unbind, constant-buffer payload upload (`Map` with
  `WRITE_DISCARD`, `UpdateSubresource` fallback), CSSetConstantBuffers,
  CSSetShader, CSSetShaderResources (up to two), CSSetUnorderedAccessViews,
  Dispatch, then unbinds the compute UAV/SRVs/shader so no bindings leak into
  the next pass.
- Copies: buffer range via `CopySubresourceRegion` with a box, whole resource
  via `CopyResource`, subresource with optional box.
- Uploads: buffer via `UpdateSubresource` box, texture via
  `UpdateSubresource` with the packet's box, row pitch and depth pitch.
- Clears and `ResolveSubresource` forward directly.
- Barrier and UAV-barrier packets are deliberately no-ops on D3D11; they stay
  in the stream for the state validation and for a future native mapping.
- The sink is compiled into `rexgpu-fh1` but is not constructed by any pass in
  M1.

### Resource-state map

`include/rex/graphics/d3d11/resource_state_tracker.h`,
`src/graphics/d3d11/resource_state_tracker.cpp`

- `unordered_map<IUnknown*, Entry>`, entry holds a `ComPtr` (frame-scoped:
  resources stay referenced until `Reset()`), the current bitmask state and a
  per-resource transition count.
- `Track` registers a known initial state without a packet.
- `Transition` emits a `D3D11BarrierPacket` only when the state changes;
  repeated same-state requests are coalesced. An untracked resource is adopted
  and still gets a barrier from `kCommon`, so the recorded stream is
  self-describing. A full tape does not update the tracked state.
- `TransitionUav` always appends a UAV barrier (unordered-access writes are not
  modelled by the state map).
- `Forget` drops one resource; `Reset` clears the map and the counters.

### Build wiring

- `src/graphics/CMakeLists.txt`: added
  `d3d11/command_list_immediate_sink.cpp` to `REXGPU_XENOS_D3D11_SOURCES`.
- `tests/unit/CMakeLists.txt`: added
  `graphics/d3d11_command_list_test.cpp` and compiled
  `command_list.cpp` / `resource_state_tracker.cpp` into `unit_tests`.
- `command_processor.cpp`: setup comment and the flag-on log line now say
  Phase 3 M1; behaviour is unchanged (services are still constructed only when
  the flag is on, and nothing records into them yet).

## Device-free tests

`tests/unit/graphics/d3d11_command_list_test.cpp` — 7 cases, 93 assertions:

1. packet order, arena payload resolution and replay through a recording sink;
2. arena alignment, out-of-range spans, cap overflow, Reset reuse;
3. packet cap, sticky overflow and Reset;
4. COM reference counts: a packet holds a reference, `Reset` releases it;
5. tracker: transition coalescing, barrier before/after values, combined read
   states, reference lifetime;
6. tracker: `Track` without a packet, untracked adoption, unconditional UAV
   barrier, null handling, Reset counters;
7. tracker: a full tape leaves the tracked state unchanged.

Build and run:

```
cmake --build out/build/win-amd64-tests --config Release --target unit_tests
out/win-amd64/Release/unit_tests.exe
```

- M1 filter: `All tests passed (93 assertions in 7 test cases)`.
- Full suite: **343 test cases — 339 passed, 4 skipped, 0 failed;
  3,754 assertions passed**.

The immediate sink itself has no device-free test: constructing an
`ID3D11DeviceContext` would require a device. Its translation unit is compiled
and linked in the game build; its packet calls are exercised indirectly by the
replay test's recording sink, which resolves the same arena spans.

## Runtime checks

Build: `.local\muse_build_game.bat` → **BUILD_EXIT=0** (the new sink and
tracker objects are under `rexgpu-fh1.dir/d3d11/`). One staging attempt failed
with "Error copying file" because the staged `rexruntime.dll` was still held
by a process; renaming that file aside and rebuilding succeeded. This is the
same file-lock/lifecycle noise described below, not a compile error.

Scenario `.local/probes/trace-short.fh1test`, state source
`.local/diag/p3m0-off.state`, 1280x720, GTX 570 by config:

- **Flag off (defaults):** result **pass**, `RUN_EXIT=0`, both captures
  `t0300.ppm` / `t0600.ppm` md5
  **`b8f68e89c6192577747ab452aa5598ef`** on six runs
  (`.local/diag/p3m1-off`, `p3m1c-off`, `p3m1e-off`, `p3m1f-off`,
  `p3m1g-off`, `p3m1j-off`; the last is the final formatted binary).
- **Flag on** (`REX_FH1_D3D11_COMMAND_LISTS=true`): result **pass**,
  `RUN_EXIT=0`, the same two md5s on three runs (`.local/diag/p3m1b-on`,
  `p3m1i-on`, `p3m1j-on`; the last is the final formatted binary). Runtime log:
  `D3D11 command lists: fh1_d3d11_command_lists is on, but recording is not
  wired yet (Phase 3 M1) - the immediate path runs unchanged`.

### Environment failures seen during the gate (not M1 code paths)

Two flag-off runs crashed, both the first run immediately after a rebuild:

- `pscrash-v1-d422dbdac0cae65bf672`, process 20620, exit `0xC0000409`
  (`FAST_FAIL_STACK_COOKIE_CHECK_FAILURE`, subcode 7), 26 s in.
- `pscrash-v1-0ac1b5583f434e0e0488`, process 6488, same exit code, 20 s in.

Both Windows Error Reporting records name `ucrtbase.dll` 10.0.26100.9711 at
`+0xA527E`; the session events stop just after
`save.upgrade_views.initialized` at observed frame 300, while the title is
still loading. M1 adds no flag-off code path, and no frame had been recorded
by the tape. Three consecutive reruns of the identical flag-off command then
passed with the baseline md5s. Recorded as a pre-existing save/profile
startup stack-cookie failure whose frequency changes with the rebuilt binary
layout/timing; not claimed as a pass and not diagnosed further in M1.

One flag-on run (`p3m1h-on`) completed and produced baseline captures but the
gate failed on a renderer error log:
`D3D11 FH1 native surfaces: texture creation failed for 0/16/1x/c10
(1280x2048x1) hr=8007000E` (`E_OUTOFMEMORY`), followed by a draw failure.
After a 15 s pause the identical run passed (`p3m1i-on`). This is consistent
with back-to-back runs exhausting the GTX 570's small pool; M1 allocates
nothing on the flag-on path except the three empty M0 services.

Defaults unchanged: `fh1_d3d11_command_lists` false,
`fh1_d3d11_resolve_readback` false, `fh1_resolve_guest_refresh` false,
`fh1_native_executor` 0.

## Not in M1

- No pass appends packets; `ResolveToMemory`, `IssueCopy`, draws, texture
  uploads and the swap path still execute immediately.
- No queue/fence use; `D3D11SubmissionQueue` remains the M0 placeholder.
- No validation of transition legality beyond the state-map/coalescing checks
  (`fh1_d3d11_command_list_validate` is still unused).

## M2 handoff

1. Split `ResolveToMemory` into prepare/execute; append the transition, dispatch
   and UAV-barrier packets while the plane tokens are still valid.
2. Make `IssueCopy` append barrier + copy packets and use `UploadBuffer` for the
   mirror upload path.
3. Open the frame tape lazily at the first recorded resolve and submit it at
   the swap boundary; compare resolve staging words and mirror hashes on vs off
   at the same swap, then keep the flag off.
