# DX12 Phase 3 M0 — Result

Status: complete. Flag and scaffolding only; recording is not wired.

## Changes

- Added `fh1_d3d11_command_lists` (bool, default **false**, restart lifecycle) and
  `fh1_d3d11_command_list_validate` (bool, default **false**, diagnostic) to
  `include/rex/graphics/flags.h` / `src/graphics/flags.cpp`.
- Added the M0 scaffolding types:
  - `include/rex/graphics/d3d11/command_list.h`, `src/graphics/d3d11/command_list.cpp`
  - `include/rex/graphics/d3d11/resource_state_tracker.h`,
    `src/graphics/d3d11/resource_state_tracker.cpp`
  - `include/rex/graphics/d3d11/submission_queue.h`,
    `src/graphics/d3d11/submission_queue.cpp`
- `D3D11CommandProcessor::SetupContext` reads the flag; when it is on, it constructs the
  three services and logs that recording is not wired. When off, nothing is constructed.
- `ShutdownContext` drops the services before the caches.
- Added the three sources to `src/graphics/CMakeLists.txt` under `REXGLUE_USE_D3D11`.
- Snapshots before editing: `*.musebak-20261026` beside `flags.h`, `flags.cpp`,
  `command_processor.h`, `command_processor.cpp`, `src/graphics/CMakeLists.txt`.

No draw, resolve, upload or swap path calls the new services in this milestone.

## Build

`.local\muse_build_game.bat` → **BUILD_EXIT=0** (60 steps, binary linked and staged).
The three new objects are present under
`out/build/win-amd64-release/rexglue-sdk/src/graphics/CMakeFiles/rexgpu-fh1.dir/d3d11/`.

## Off-path verification (defaults, no cvars)

`py -3.11 tools/run-fh1-render-test.py .local/probes/trace-short.fh1test
--state-root .local/diag/trace14.log.state --output .local/diag/p3m0-off --hidden`

- RUN_EXIT=0, `"result": "pass"`.
- `t0300.ppm` md5 **b8f68e89c6192577747ab452aa5598ef**
- `t0600.ppm` md5 **b8f68e89c6192577747ab452aa5598ef**
- Both match the recorded baseline. No command-list log line in the runtime log.

## Flag-on inertness check

Same command with `REX_FH1_D3D11_COMMAND_LISTS=true`:

- RUN_EXIT=0, `"result": "pass"`.
- Both captures md5 **b8f68e89c6192577747ab452aa5598ef** — the flag does not change the
  frame.
- Runtime log (`.local/diag/p3m0-on.state/logs/runtime.log:22`):
  `D3D11 command lists: fh1_d3d11_command_lists is on, but recording is not wired yet
  (Phase 3 M0) - the immediate path runs unchanged`.
- No such line in the off run.

## Not run

The game preset configures `REXGLUE_BUILD_TESTS=OFF` and has no `unit_tests` target, so
`unit_tests "d3d11*"` could not be run in this tree. (M1 correction: the rexglue tests
build at `.local/rexglue/out/build/win-amd64-tests` does configure and build
`unit_tests`; M1 used it. The M0 statement is about the game preset only.) The new translation units compile in
the game build; M1 adds the first tests that exercise them.

## Defaults

`fh1_native_executor` 0, `fh1_d3d11_resolve_readback` false,
`fh1_resolve_guest_refresh` off, `fh1_d3d11_command_lists` false,
`fh1_d3d11_command_list_validate` false. Phase 13 files untouched.

## Next

M1: packet stream, arena, COM references, replay sink, tracker map and validation, with
unit tests that need no device. Recording remains unwired until those tests pass.
