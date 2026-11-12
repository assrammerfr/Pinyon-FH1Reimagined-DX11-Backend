# Pinyon-FH1Reimagined-DX11-Backend
This is a dx11 backend to optimize the living hell out of Pinyon shift and Fh1 Reimagined recomps. built off a gtx 570 so this is gonna be optimized to its fullest capacity,

## What this is

A Direct3D 11 graphics backend for Pinyon Shift — the Forza Horizon 1
recompiler / ReImagined runtime. It exists for one reason: the target card
has no D3D12 at all. The GeForce GTX 570's last driver (391.35) tops out at
feature level 11_0, so the game needs a plain D3D11 path to run on it. This
backend is that path, tuned to the 570's limits rather than written around
them.

## Target hardware

- **EVGA GeForce GTX 570 HD Superclocked** (GF110, part 012-P3-1572)
- **1.28 GB VRAM** — the binding constraint on the whole backend
- **Driver 391.35** — the last Fermi driver; no D3D12, no Vulkan
- **Feature level 11_0** is the card's ceiling
- **4-phase VRM** (compact, cost-down non-reference PCB) — keep total power
  under ~240–250 W and core voltage under 1.075 V; the gates run at stock
  1.000 V

Full rationale: [`docs/hardware-constraints.md`](docs/hardware-constraints.md).

## How it works

The guest (Xbox 360) EDRAM is modelled as a 2048-tile grid. Front-buffer
tiles live at 720..1439 (base 720, pitch 16, 45x16 tiles). Surfaces are keyed
by base / pitch / MSAA / kind, and per-tile ownership (`Fh1EdramTiles`)
enforces one writer per address; claims follow the draw rect. Transfers go
through `CopySubresourceRegion` or SM 5.0 shaders. Depth goes through an
EDRAM words pass that binds its words with typed `R32_UINT` views, one
element per guest EDRAM word — FL 11_0 / Fermi refuses raw UAVs. The
front-buffer resolve is a full rect, 1280x720; the presenting resolve is
pack=1, fmt 54.

Guest shaders arrive stamped SM 5.1 and are retargeted to SM 5.0, because
FL 11_0 rejects 5.1 outright. With 1.28 GB of VRAM, EDRAM render targets
are pooled and aliased aggressively instead of each guest surface getting
its own host texture.

## Status

- **Phase 1 gate — PASS.** 4,165 cleared frames / 70 s, median 59.98 FPS
  on the GTX 570.
- **Phase 2 gate — PASS.** 4,122 frames / 70 s, 17,000 texture uploads,
  0 failures.
- **Colour fix — DONE.** The R↔B channel swap is fixed (upload-side swap
  restored in `texture_cache.cpp`). The neon blue cast is gone — frames are
  warm/red. Still needs a clean "good run" for a definitive sunset-gold
  proof (every free-roam run so far was a bad run with the black band).
- **Optimization — DONE.** Median free-roam 9.0 → 13.8–15.4 FPS (+53–71%)
  by skipping the GPU wait + readback per texture load when no new resolve
  landed since the last refresh.
- **Stutter fix — DONE.** Spike untile halved (75.5 → 37 ms) via a fast
  path in `CopyGuestLevelSlice` that computes the block offset once per run
  instead of once per block. p95 274 → 232–235 ms, p99 333–384 → 276–291 ms,
  median 16.1–18.8 FPS. Stutter reduced but not eliminated — p95 is still
  ~233 ms vs 33 ms needed for 30 FPS.
- **Refresh fix — IN PROGRESS.** The remaining stutter is the refresh
  (~117 ms in spike frames: GPU waits ~73 ms + readback ~43 ms). Fix: batch
  the refreshes into 2–3 waits, defer the readback via the copy engine with
  double-buffered staging, and only refresh ranges that actually changed
  (dirty tracking).
- **Current.** The game runs and the world renders (road, trees, animals,
  headlights). A black band above row 360 still shows up on some runs —
  runs are wholly good or wholly bad. The acceptance criterion (no band
  above row 360 across two seeds and repeated swaps) is not met.
- **Baseline:** `trace-short` on defaults reproduces md5
  `b8f68e89c6192577747ab452aa5598ef` on both captures. If that hash moves,
  the default path changed.

The running log is [`docs/DX11_FINISH_NOTES.md`](docs/DX11_FINISH_NOTES.md)
— it is updated at every step boundary and is the source of truth for
where the work stands.

## Layout

- `src/graphics/d3d11/` — the backend implementation (17 .cpp files:
  command processor/list, executor, resolve, surfaces, graphics system,
  pipeline/texture/render-target caches, shader, shared memory, submission
  queue, resource state tracker, …)
- `include/rex/graphics/d3d11/` — the matching headers (17 .h files)
- `include/rex/graphics/fh1_edram_tiles.h`,
  `include/rex/graphics/fh1_edram_surfaces.h` — EDRAM tile ownership and
  surface keys
- `docs/DX11_FINISH_NOTES.md` — running log, phase by phase
- `docs/hardware-constraints.md` — why the backend is shaped the way it is
- `docs/dx12-phase0/` — the D3D12 phase 0–3 inventory and results (context
  for where this backend came from)
- `tools/build-d3d11.bat` — Release build from the Pinyon checkout
- `tools/check_d3d11_gate.py` — gate checker

## Build

These files live in the Pinyon Shift tree (`rexglue`) at the matching
paths — `src/graphics/d3d11/` and `include/rex/graphics/d3d11/` under the
checkout. Drop them into a Pinyon checkout at those paths, then build the
Release config from the checkout:

```bat
tools\build-d3d11.bat
```

That runs VS 2022 BuildTools (`VsDevCmd.bat -arch=amd64`) and
`cmake --build <checkout>\out\build\win-amd64 --config Release`. The
checkout's canonical location is `E:\PinyonShift\work\.local\rexglue`;
the build script carries that path. A clean rebuild is 1225 steps and
exits 0.

## Known gaps (not bugs to work around silently)

- **Multisampled depth cannot be read back on D3D11.** Staging cannot
  take MSAA and `ResolveSubresource` has no depth support. That is an open
  instrument gap.
- **The black band.** Still intermittent on bad runs (above row 360). See
  Status above. The upload/untile leg is excluded (paired same-swap
  captures are band-for-band identical), and late composite draws are
  excluded too — the top half is simply produced only on some swaps. The
  next experiment is a guest probe comparing guest and mirror bytes of the
  sampled texture in a known-good vs known-bad run.
