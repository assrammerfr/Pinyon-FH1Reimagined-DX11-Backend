# Hardware constraints — GTX 570 (Fermi, FL 11_0)

This backend is built and validated against one target card, and its design
choices follow from that card's limits. The backend is plain D3D11, so it
also runs on other D3D11 GPUs (it has been discussed for the RX 5700 XT),
but the 570 is the floor it is tuned for.

## Target card

- **EVGA GeForce GTX 570 HD Superclocked** (part 012-P3-1572), GF110
- **1.28 GB VRAM** — the binding constraint on the whole backend
- **Driver 391.35** — the last Fermi driver; no D3D12, no Vulkan
- **Feature level 11_0** is the card's ceiling
- **4-phase VRM** (compact, cost-down non-reference PCB) — keep total power
  under ~240–250 W and core voltage under 1.075 V; the gates run at stock
  1.000 V

## Consequences for the code

- **EDRAM target pooling / aliasing.** With 1.28 GB of VRAM, EDRAM render
  targets are pooled and aliased aggressively instead of each guest surface
  getting its own host texture. Surfaces that alias the same EDRAM address
  are tracked by tile ownership (`Fh1EdramTiles`), one writer per address.
- **Typed R32_UINT views, no raw UAVs.** The EDRAM words pass (depth
  destinations) binds its words through typed `R32_UINT` views, one element
  per guest EDRAM word. Feature level 11_0 / Fermi 391.35 stores raw UAVs
  reliably only at low offsets and refuses a sub-range raw view outright.
- **SM 5.0 retarget.** Guest shaders arrive stamped SM 5.1; FL 11_0 rejects
  them, so shaders are retargeted to SM 5.0.
- **No D3D11.1+ features.** Everything the backend uses must exist at
  FL 11_0. Multisampled depth cannot be read back on D3D11 (staging cannot
  take MSAA, and `ResolveSubresource` has no depth support) — that is a
  known open instrument gap, not a bug to work around silently.

## Validation

- Phase 1 gate: 4,165 cleared frames / 70 s, median 59.98 FPS on the 570.
- Phase 2 gate: 4,122 frames / 70 s, 17,000 texture uploads, 0 failures.
- Current state is Phase 13B (see `docs/DX11_FINISH_NOTES.md`): the
  presenting frame has a black band above row 360 while the composite
  underneath holds the full world. That defect is open and is the current
  work item — it is not papered over here.
