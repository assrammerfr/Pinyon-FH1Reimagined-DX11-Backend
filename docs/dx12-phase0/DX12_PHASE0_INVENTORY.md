# DX12 Phase 0 — DX11 Call Inventory

Status: Phase 0 only. No translation code, no slice definition, no default changes.
Generated: 2026-10-26 (UTC), from a live read of `F:\PinyonShift\work`.
Output folder: `F:\PinyonShift\dx12-phase0`.

## 0. Tree verification (done before inventorying)

- `F:\PinyonShift\work\.local\rexglue` exists (verified with `ls -d`).
- D3D11 translation units, graphics side (`src/graphics/d3d11/`, 13 files):
  `command_processor.cpp`, `fh1_native_executor.cpp`, `fh1_native_resolve.cpp`,
  `fh1_native_surfaces.cpp`, `graphics_system.cpp`, `host_render_config.cpp`,
  `pipeline_cache.cpp`, `primitive_processor.cpp`, `render_target_cache.cpp`,
  `shader.cpp`, `shared_memory.cpp`, `shared_memory_upload_device.cpp`, `texture_cache.cpp`.
- D3D11 headers (`include/rex/graphics/d3d11/`, 14 files):
  `command_processor.h`, `fh1_native_executor.h`, `fh1_native_resolve.h`,
  `fh1_native_surfaces.h`, `graphics_system.h`, `host_render_config.h`,
  `pipeline_cache.h`, `primitive_processor.h`, `render_target_cache.h`, `shader.h`,
  `shared_memory.h`, `shared_memory_upload.h`, `shared_memory_upload_device.h`,
  `texture_cache.h`.
- D3D11 UI side (`src/ui/d3d11/`, 3 files): `d3d11_immediate_drawer.cpp`,
  `d3d11_presenter.cpp`, `d3d11_provider.cpp`.
- UI headers (`include/rex/ui/d3d11/`): `d3d11_api.h`, `d3d11_immediate_drawer.h`,
  `d3d11_presenter.h`, `d3d11_provider.h`.
- Backups (`*.musebak*`) exist beside some files and are excluded from every count.
- These are the compiled D3D11 sources: `src/graphics/CMakeLists.txt:125-151`
  (`REXGLUE_USE_D3D11`) and `src/ui/CMakeLists.txt:70-75`.
  The live build log `.local/muse_build_game.log:192` reads
  `-- Graphics: D3D12=ON D3D11=ON Vulkan=ON`.
- No git repository is used; `.musebak` files are the backups.

Method: comment- and string-stripped scan of the compiled translation units for
`receiver->Method(` calls, then receiver-type resolution against the declared
`ID3D11*`/`IDXGI*` members and locals, with chained-expression calls
(`GetD3D11Provider().GetDevice()->X`, `paint_context.swap_chain->X`) resolved by hand.
Counts are call sites in the compiled TUs, not unique runtime paths. The two name-match
corrections are stated in section 5.

## 1. Counts by group

| Group | Distinct entry points | Call sites |
|---|---|---|
| Device/context setup, swap chain, lifetime | 21 | 93 |
| Resources, views, transfers | 17 | 137 |
| Shaders and pipeline state | 25 | 112 |
| Draw/dispatch and bind calls | 12 | 52 |
| **Backend total** | **75** | **394** |
| Adjacent shared UI DXGI (not backend-specific) | +4 new | +6 |
| **Combined** | **79** | **400** |

Distinct entry points are counted by API method name: `GetDesc`, `QueryInterface` and
`Release` each cover more than one interface (texture/SRV/resource/output; factory/swap
chain; every COM object).

## 2. Device/context setup, swap chain, lifetime — 93 sites

| API | Sites | First site | Purpose |
|---|---:|---|---|
| `D3D11CreateDevice` | 1 | `src/ui/d3d11/d3d11_provider.cpp:166` | Create the single device at feature level 11_0 on the selected adapter. |
| `CreateDXGIFactory1` | 2 (+1 shared) | `src/ui/d3d11/d3d11_provider.cpp:62` | DXGI factory for adapter enumeration and swap-chain creation. |
| `QueryInterface` | 2 | `src/ui/d3d11/d3d11_presenter.cpp:684` | Factory1→Factory2; SwapChain1→SwapChain3. |
| `EnumAdapters1` | 1 | `src/ui/d3d11/d3d11_provider.cpp:112` | Enumerate adapters; selection is by description substring (`d3d11_adapter`), never by index. |
| `GetDesc1` | 3 | `src/ui/d3d11/d3d11_provider.cpp:115` | Adapter description/vendor/memory; swap-chain description. |
| `CreateSwapChainForHwnd` | 1 | `src/ui/d3d11/d3d11_presenter.cpp:676` | Flip-discard → flip-sequential → blt swap-chain tiers for an HWND. |
| `MakeWindowAssociation` | 1 | `src/ui/d3d11/d3d11_presenter.cpp:703` | Disable Alt+Enter handling. |
| `GetImmediateContext` | 1 | `src/graphics/d3d11/fh1_native_surfaces.cpp:163` | One-time context for surface-creation clears (the provider supplies the context everywhere else). |
| `CheckMultisampleQualityLevels` | 2 | `src/graphics/d3d11/host_render_config.cpp:66` | Probe 2x/4x support per render-target format. |
| `GetDeviceRemovedReason` | 5 | `src/graphics/d3d11/fh1_native_executor.cpp:1262` | Device-lost diagnostics after a failed create/copy. |
| `ClearState` | 1 | `src/ui/d3d11/d3d11_provider.cpp:75` | Unbind everything at provider teardown. |
| `Flush` | 4 | `src/graphics/d3d11/command_processor.cpp:3346` | Flush queued work before waits/present. |
| `CreateQuery` | 1 | `src/graphics/d3d11/command_processor.cpp:3121` | `D3D11_QUERY_EVENT` used as the GPU-wait fence. |
| `End` | 1 | `src/graphics/d3d11/command_processor.cpp:3126` | End the event query. |
| `GetData` | 1 | `src/graphics/d3d11/command_processor.cpp:3128` | Spin until the event query reports completion. |
| `GetCurrentBackBufferIndex` | 1 | `src/ui/d3d11/d3d11_presenter.cpp:330` | Flip-model back-buffer index. |
| `GetBuffer` | 1 | `src/ui/d3d11/d3d11_presenter.cpp:732` | Acquire the back-buffer texture. |
| `Present` | 1 | `src/ui/d3d11/d3d11_presenter.cpp:505` | Present with the vsync flag. |
| `ResizeBuffers` | 1 | `src/ui/d3d11/d3d11_presenter.cpp:606` | Resize on surface-size change. |
| `Release` | 61 | `src/graphics/d3d11/command_processor.cpp:3350` | COM teardown across every interface the backend owns. |
| `AddRef` | 1 | `src/graphics/d3d11/texture_cache.cpp:811` | Keep a cached SRV alive for the bindful path. |

Adapter/device details: `D3D11Provider::Initialize` (`d3d11_provider.cpp:91-190`) creates
Factory1, QIs Factory2, enumerates adapters (skipping `DXGI_ADAPTER_FLAG_SOFTWARE`),
matches the `d3d11_adapter` description (the live target is `GTX 570`), then calls
`D3D11CreateDevice` with `D3D_DRIVER_TYPE_UNKNOWN`, `D3D_FEATURE_LEVEL_11_0`,
`D3D11_CREATE_DEVICE_BGRA_SUPPORT`, no debug flag, and rejects any created feature level
other than 11_0. One device, one immediate context, no deferred contexts.

## 3. Resources, views, transfers — 137 sites

| API | Sites | First site | Purpose |
|---|---:|---|---|
| `CreateBuffer` | 16 | `src/graphics/d3d11/command_processor.cpp:1096` | Constant buffers, staging/readback buffers, the 512 MB mirror, probe buffers. |
| `CreateTexture2D` | 14 | `src/graphics/d3d11/command_processor.cpp:173` | Resolve/readback staging, EDRAM surfaces, UI images. |
| `CreateTexture3D` | 1 | `src/graphics/d3d11/texture_cache.cpp:473` | Guest 3D textures. |
| `CreateShaderResourceView` | 9 | `src/graphics/d3d11/fh1_native_executor.cpp:1278` | Raw/typed buffer views, EDRAM SRVs, texture-cache SRVs. |
| `CreateRenderTargetView` | 6 | `src/graphics/d3d11/fh1_native_executor.cpp:1576` | EDRAM colour targets (UNORM view and raw UINT view) and UI targets. |
| `CreateDepthStencilView` | 1 | `src/graphics/d3d11/fh1_native_surfaces.cpp:122` | EDRAM depth targets. |
| `CreateUnorderedAccessView` | 5 | `src/graphics/d3d11/fh1_native_executor.cpp:1271` | Raw UAVs for resolves, word transfers and probes. |
| `GetDesc` | 18 | `src/graphics/d3d11/command_processor.cpp:164` | Texture/SRV/resource descriptions before copies and dumps. |
| `GetResource` | 2 | `src/graphics/d3d11/texture_cache.cpp:841` | SRV → resource for format/desc queries. |
| `Map` | 20 | `src/graphics/d3d11/command_processor.cpp:179` | Dynamic constants (`WRITE_DISCARD`), staging writes and readbacks. |
| `Unmap` | 22 | `src/graphics/d3d11/command_processor.cpp:212` | Close every map. |
| `UpdateSubresource` | 3 | `src/graphics/d3d11/command_processor.cpp:1105` | Constant-buffer updates and the texture cache's untiled uploads. |
| `CopySubresourceRegion` | 8 | `src/graphics/d3d11/command_processor.cpp:177` | Staging→texture, texture→staging, staging→mirror box copies. |
| `CopyResource` | 5 | `src/graphics/d3d11/command_processor.cpp:2682` | Whole-surface staging copies before resolve/readback. |
| `ResolveSubresource` | 2 | `src/graphics/d3d11/command_processor.cpp:283` | MSAA colour/depth sample resolve into a single-sample texture. |
| `ClearRenderTargetView` | 4 | `src/graphics/d3d11/command_processor.cpp:1606` | Surface-creation clears, diagnostics, UI back-buffer clear. |
| `ClearDepthStencilView` | 1 | `src/graphics/d3d11/fh1_native_surfaces.cpp:170` | Depth/stencil clear on surface creation (24_8 to 1.0, 24_FLOAT to 0.0). |

## 4. Shaders and pipeline state — 112 sites

| API | Sites | First site | Purpose |
|---|---:|---|---|
| `CreateVertexShader` | 4 | `src/graphics/d3d11/fh1_native_executor.cpp:1701` | Fullscreen transfer VS and UI VS from DXBC. |
| `CreatePixelShader` | 8 | `src/graphics/d3d11/fh1_native_executor.cpp:1409` | Transfer pixel shaders; guest pixel shaders. |
| `CreateComputeShader` | 4 | `src/graphics/d3d11/fh1_native_executor.cpp:1322` | Word transfer/resolve compute shaders. |
| `CreateGeometryShader` | 1 | `src/graphics/d3d11/pipeline_cache.cpp:778` | Quad-list → triangle-strip geometry shader. |
| `CreateDepthStencilState` | 5 | `src/graphics/d3d11/fh1_native_executor.cpp:1473` | Depth-write/compare/stencil variants. |
| `CreateRasterizerState` | 3 | `src/graphics/d3d11/fh1_native_executor.cpp:1729` | Cull/fill modes for transfers and draws. |
| `CreateBlendState` | 2 | `src/graphics/d3d11/pipeline_cache.cpp:867` | Guest blend combinations (cached). |
| `CreateSamplerState` | 2 | `src/graphics/d3d11/command_processor.cpp:1188` | Sampler cache from the guest fetch. |
| `IASetInputLayout` | 5 | `src/graphics/d3d11/fh1_native_executor.cpp:791` | Always `nullptr`: the backend uses no input layouts; vertex data is fetched from buffers. |
| `VSSetShader` | 6 | `src/graphics/d3d11/fh1_native_executor.cpp:794` | Vertex-stage bind. |
| `PSSetShader` | 7 | `src/graphics/d3d11/fh1_native_executor.cpp:795` | Pixel-stage bind. |
| `CSSetShader` | 5 | `src/graphics/d3d11/fh1_native_executor.cpp:1034` | Compute-stage bind. |
| `GSSetShader` | 1 | `src/graphics/d3d11/pipeline_cache.cpp:895` | Geometry-stage bind (quad list). |
| `VSSetConstantBuffers` | 3 | `src/graphics/d3d11/command_processor.cpp:1273` | Vertex constants (4 slots). |
| `PSSetConstantBuffers` | 6 | `src/graphics/d3d11/command_processor.cpp:1274` | Pixel constants (4 slots). |
| `CSSetConstantBuffers` | 2 | `src/graphics/d3d11/fh1_native_executor.cpp:1035` | Compute constants. |
| `VSSetShaderResources` | 3 | `src/graphics/d3d11/command_processor.cpp:1280` | Vertex-stage SRVs (mirror raw buffer, textures). |
| `PSSetShaderResources` | 13 | `src/graphics/d3d11/command_processor.cpp:1281` | Pixel-stage SRVs (guest textures, mirror, UI source). |
| `CSSetShaderResources` | 4 | `src/graphics/d3d11/fh1_native_executor.cpp:1038` | Compute-stage source views for transfers/resolve. |
| `CSSetUnorderedAccessViews` | 4 | `src/graphics/d3d11/fh1_native_executor.cpp:1039` | Compute destinations (word buffer, mirror/staging). |
| `VSSetSamplers` | 1 | `src/graphics/d3d11/command_processor.cpp:1311` | Vertex samplers. |
| `PSSetSamplers` | 3 | `src/graphics/d3d11/command_processor.cpp:1313` | Pixel samplers. |
| `OMSetBlendState` | 6 | `src/graphics/d3d11/command_processor.cpp:861` | Blend state + factor. |
| `OMSetDepthStencilState` | 8 | `src/graphics/d3d11/command_processor.cpp:877` | Depth/stencil state + ref. |
| `RSSetState` | 6 | `src/graphics/d3d11/fh1_native_executor.cpp:790` | Rasterizer state. |

Shader production is runtime DXBC translation, not a prebuilt pack: the pipeline cache
calls the translator at `src/graphics/d3d11/pipeline_cache.cpp:327`, then the stage
objects are created in `shader.cpp:32-105` and `fh1_native_executor.cpp:1322-1409`.

## 5. Draw/dispatch and binds — 52 sites

| API | Sites | First site | Purpose |
|---|---:|---|---|
| `Draw` | 7 | `src/graphics/d3d11/command_processor.cpp:2410` | Guest non-indexed draws and 3-vertex fullscreen transfer passes. |
| `DrawIndexed` | 1 | `src/graphics/d3d11/command_processor.cpp:2450` | Guest indexed draws. |
| `Dispatch` | 3 | `src/graphics/d3d11/fh1_native_executor.cpp:1057` | Compute transfers and the memory resolve. |
| `IASetPrimitiveTopology` | 6 | `src/graphics/d3d11/command_processor.cpp:2406` | Point/line/strip/list mapping; quad list is served by the GS. |
| `IASetVertexBuffers` | 3 | `src/graphics/d3d11/fh1_native_executor.cpp:793` | Transfer passes bind zero vertex buffers; guest fetches use buffers/SV_VertexID. |
| `IASetIndexBuffer` | 1 | `src/graphics/d3d11/command_processor.cpp:2449` | Host-converted, builtin, or guest-DMA indices (the mirror is index-bindable). |
| `RSSetViewports` | 9 | `src/graphics/d3d11/command_processor.cpp:834` | Guest viewport and full-target passes. |
| `RSSetScissorRects` | 8 | `src/graphics/d3d11/command_processor.cpp:846` | Guest scissor. |
| `OMSetRenderTargets` | 11 | `src/graphics/d3d11/command_processor.cpp:809` | Colour RTVs + optional DSV; unbound before compute resolves. |
| `CSGetShaderResources` | 1 | `src/graphics/d3d11/fh1_native_resolve.cpp:313` | Save compute bindings before a resolve. |
| `CSGetConstantBuffers` | 1 | `src/graphics/d3d11/fh1_native_resolve.cpp:317` | Save compute constants. |
| `CSGetUnorderedAccessViews` | 1 | `src/graphics/d3d11/fh1_native_resolve.cpp:315` | Save compute UAVs. |

Count corrections (name-match scan → API call sites):
`GetImmediateContext` 39 → **1** (the other 38 are the project's
`D3D11Provider::GetImmediateContext()` accessor); `Draw` 9 → **7** (one definition and
one declaration in the header are not calls). Every other count above is a call site.

## 6. Mechanisms (not just calls)

These are the pieces a DX12-style equivalent must reproduce. Each entry states where it
lives and the contract it must preserve; no translation design is proposed here.

### M1. EDRAM surface registry
Where: `include/rex/graphics/fh1_edram_surfaces.h:15-87`,
`src/graphics/d3d11/fh1_native_surfaces.cpp:69-185`.
- A guest EDRAM surface is identified by `Fh1SurfaceKey` = base tile | pitch tile count |
  MSAA sample count | depth flag | storage format. Height is derived from the pitch to the
  next EDRAM addressing period, clamped by the guest texture limit and the host limit
  (`Fh1SurfaceHeight`).
- One host `ID3D11Texture2D` per key, with up to five views: colour RTV + raw UINT RTV,
  depth DSV, colour/depth SRV, and a stencil SRV for depth. Sample count is 1x/2x/4x;
  depth formats are D24S8/D32_FLOAT_S8X24; colour storage and draw formats are separated
  (typed resource vs UNORM draw view).
- Surfaces are created on demand and cleared at creation (`ClearRenderTargetView` or
  `ClearDepthStencilView` + `Flush`). `FindSurface`/`GetOrCreateSurface` are the only
  registry access points.
- Contract: a resolve's source is identified by key + rectangle, and the registry, not the
  guest address, owns the content. Tile ownership is resolved by
  `ResolveSourceOwner` (`command_processor.cpp:2633`) and the resolve plan.

### M2. Native resolve to guest memory
Where: `src/graphics/d3d11/fh1_native_resolve.cpp:181-369`,
`src/graphics/d3d11/command_processor.cpp:2528-3100`.
- `IssueCopy` plans one resolve per source surface from the guest copy registers
  (`Fh1PlanResolve`), chooses the source owner surface, then `ResolveToMemory` dispatches
  one SM5.0 compute shader that reads the surface's SRV(s) and writes packed destination
  words through a raw UAV.
- Destination target order: a dedicated resolve staging buffer (default), a mirror window,
  the whole mirror, or a small probe buffer, selected by diagnostic flags. The default is
  staging because a raw UAV store at the front buffer's address (~470 MB into the mirror)
  does not land on this driver; the extent is then copied into the mirror at its guest
  address with `CopySubresourceRegion` (a copy-engine operation with no shader-address
  limit).
- Before binding the source SRV, the output-merger targets are unbound
  (`OMSetRenderTargets(... no targets ...)`): D3D11 silently drops an SRV that is still
  bound as a render target on the same resource, which previously produced zero-filled
  resolves. One-use SRVs are created per resolve and released.
- Readback: `Map`/`Unmap` on a staging texture or the readback buffer,
  `CopyReadbackToGuest` for pages the GPU owns; `WaitForGpu` (event query) before reads.
- Contract: the resolve destination is a guest byte range with a format/pack/endian/pitch
  description, not a host texture. Resolve ordering, unbinding, and fence points are part
  of the contract.

### M3. MSAA sample resolve
Where: `src/graphics/d3d11/command_processor.cpp:283,328`.
- `ResolveSubresource` converts an MSAA colour/depth surface into a single-sample texture
  for the readback path, using the format's resolve capability; the native compute resolve
  separately handles sample selection for the guest-visible copy.
- Contract: MSAA surfaces are host textures with SampleDesc.Count > 1; sample selection
  and the 1x destination are explicit.

### M4. Guest-memory mirror
Where: `include/rex/graphics/shared_memory.h:30-31` (2^29 bytes = 512 MiB),
`src/graphics/d3d11/shared_memory.cpp:33-84`,
`src/graphics/d3d11/shared_memory_upload_device.cpp:31-543`.
- One committed 512 MiB default buffer (`D3D11_USAGE_DEFAULT`,
  `BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS | BIND_INDEX_BUFFER`,
  `RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS`) holds the entire guest GPU address space mirror.
  There are no sparse/tiled resources at feature level 11_0, so the full range is
  committed.
- CPU→mirror uploads go through a 4 MiB staging buffer (`kSharedMemoryUploadChunkBytes = 1 << 22`): `Map(WRITE)`, `memcpy`, `Unmap`,
  then `CopySubresourceRegion` chunks into the mirror. Pages are marked valid before the
  copy so a racing CPU write invalidates them and forces a re-upload.
- Mirror→CPU readbacks use a read-only staging buffer. GPU-written pages are tracked so a
  CPU upload never overwrites them (`CopyReadbackToGuest`, `CountValidPages`,
  `MarkRangeAsResolved`).
- Raw SRV/UAV views serve vertex fetch (`t0` ByteAddressBuffer), guest texture reads,
  resolve destinations, index buffers for guest-DMA draws, and probe windows.
- Contract: a 512 MiB byte-addressable mirror with CPU-valid page tracking, chunked
  upload/readback, and raw buffer views at arbitrary offsets (except the driver's
  large-offset UAV limitation that forces the staging copy in M2).

### M5. Tiled texture cache
Where: `src/graphics/d3d11/texture_cache.cpp:437-505` (create),
`517-694` (load/untile), `709-730` (swap extent/texture), `815-893` (guest-base lookup,
bindful SRV), `1012` (sampler desc).
- A guest texture key (base, format, tiling, dimensions, mip count, endianness) maps to a
  host `ID3D11Texture2D`/`Texture3D` with 1..N mips, array/cube/3D dimension, and an SRV.
- Untiling and format conversion are CPU-side: `CopyGuestLevelSlice` writes a linear
  staging image (256-byte row pitch), then `UpdateSubresource` uploads each mip/slice.
  There is no compute untiling and no mip generation.
- Variant SRVs (unsigned/signed, stacked-as-2D, cube) are created on demand; sampler state
  is cached from the guest fetch (address modes, filter, anisotropy).
- Unsupported formats are dropped: DXT/4:2:2 decompression and scaled resolves are not on
  this path (`CreateTexture` returns null and logs once).
- Contract: guest tiling/endianness/format swizzle are consumed on the CPU; the host
  texture is linear and single-sample; the cache owns LRU and guest-base lookup.

### M6. Shaders and pipeline state
Where: `src/graphics/d3d11/shader.cpp:32-105`,
`src/graphics/d3d11/pipeline_cache.cpp:327,778-895`,
`src/graphics/pipeline/shader/dxbc_translator.cpp` (compiled into this backend by
`src/graphics/CMakeLists.txt:141-146`).
- Guest shaders are translated to DXBC at runtime (DXBC container + translator); the
  pipeline cache stores per-modification translations, creates VS/PS/GS objects, and
  caches blend/depth/raster states keyed by guest state.
- Geometry shaders exist only for emulating quad lists; there are no hull/domain stages.
- The input layout is always null; guest vertex data is fetched through buffers and the
  translated VS, not IA vertex declarations.
- Contract: DXBC bytecode is the input to `Create*Shader`; the translator is a producer,
  not a D3D11 call site. Shader-pack/DXC work is a separate track (section 7).

### M7. Draw path and primitive processing
Where: `src/graphics/d3d11/command_processor.cpp:2377-2451`,
`src/graphics/d3d11/primitive_processor.cpp:45-160`,
`src/graphics/d3d11/fh1_native_executor.cpp:690-824`.
- The command processor decodes the guest draw, asks the primitive processor for host
  vertex/index buffers (guest-DMA indices bind directly to the mirror; converted or
  builtin indices use host buffers), sets topology/viewport/scissor, binds
  shaders/SRVs/samplers/constant buffers and render targets, then issues `Draw` or
  `DrawIndexed`.
- The native executor issues fullscreen passes with `Draw(3)` and compute transfers with
  `Dispatch`, plus its own OM/RS/shader binds.
- Contract: single immediate context, explicit state binds per pass, guest-DMA indices
  read the mirror, and the bind sequence matters (OM must be unbound before a resolve SRV).

### M8. Present and guest-output blit
Where: `src/ui/d3d11/d3d11_presenter.cpp:235-319` (guest output blit), `328-380`
(back-buffer paint), `496-510` (present), `606-746` (swap chain, resize, buffer acquire),
`src/ui/d3d11/d3d11_immediate_drawer.cpp:24-60`.
- Swap chain: Factory2 → `CreateSwapChainForHwnd` with tiered fallback
  (`FLIP_DISCARD`, 3 buffers → `FLIP_SEQUENTIAL`, 3 → `DISCARD`/blt, 1), BGRA format,
  `SCALING_NONE` when sizes match else `STRETCH`; SwapChain1 is QI'd to SwapChain3.
- Present path: acquire the current back buffer (`GetBuffer`), create/bind its RTV, clear,
  draw the guest output through a rect+texture shader (`Draw(4)`), then `Present`.
  `PresentGuestOutputTexture` blits a source SRV into the guest-output RTV with a
  full-image rect and bilinear sampler. `ResizeBuffers` on size change.
- Device loss is detected from `DXGI_ERROR_DEVICE_REMOVED`/`RESET` and surfaced through a
  host-GPU-loss callback.
- Contract: a flip-model swap chain with a blt fallback, one back-buffer RTV per acquired
  buffer, a separate guest-output texture/RT, and a full-image blit before present.

### M9. GPU wait / event query
Where: `src/graphics/d3d11/command_processor.cpp:3113-3130`.
- `WaitForGpu` lazily creates a `D3D11_QUERY_EVENT`, calls `End`, then spins on `GetData`
  until completion; it is used before readbacks, resolve probes and the swap readback.
- Contract: a fence point the CPU can observe; every GPU-written range read by the CPU or
  copied to guest memory passes through it.

### M10. Host render configuration
Where: `src/graphics/d3d11/host_render_config.cpp:40-210`.
- Maps Xenos colour/depth formats to DXGI resource/draw/SRV formats, decides MSAA 2x/4x
  support with `CheckMultisampleQualityLevels`, handles fixed-16 truncation, gamma
  render targets, and depth-float24 rounding.
- Contract: format capability tables and sample-count decisions are host-driver facts,
  not guest facts.

### M11. Shared UI DXGI tick (adjacent, not backend-specific)
Where: `src/ui/presenter.cpp:1442-1697`, `include/rex/ui/presenter.h:1083-1114`.
- Calls: `CreateDXGIFactory1` (1), `IDXGIFactory1::IsCurrent` (1),
  `IDXGIFactory1::EnumAdapters` (1), `IDXGIAdapter::EnumOutputs` (1),
  `IDXGIOutput::GetDesc` (1), `IDXGIOutput::WaitForVBlank` (1) — 6 sites, 4 entry points
  not otherwise in the backend list.
- A worker thread waits for the monitor's vertical blank through the DXGI output of the
  adapter that owns the window; the factory is recreated when the monitor set changes.
- This path is shared by every backend (Vulkan/D3D12 included), so it is listed in the
  inventory but not attributed to the D3D11 backend. It must exist for the DX12-style path
  too; wrapping it as backend-specific would be wrong.

## 7. Out of scope

Rule for later phases: nothing outside this inventory gets a DX12-style wrapper. If a
needed call is missing here, it is added to this inventory first.

### 7a. Headers included but their unique APIs are never called
Verified by grep over the compiled D3D11 TUs: 0 call sites for every name below.

- `d3dcompiler.h`: `D3DCompile`, `D3DReflect`, `D3DDisassemble`,
  `D3DGetInputSignatureBlob`, `D3DStripShader`, `D3DCreateBlob`. Shader bytecode arrives
  from the project's own DXBC translator, not the D3DCompiler runtime.
- `d3d11sdklayers.h` / `dxgidebug.h`: `ID3D11Debug`, `DXGIGetDebugInterface1`,
  `D3D11_CREATE_DEVICE_DEBUG`. The device is created without the debug layer.
- `dxgi1_6.h`: only `IDXGIFactory1/2`, `IDXGIAdapter1`, `IDXGISwapChain1/3`,
  `IDXGIOutput` are used. No `IDXGIFactory3-6`, `IDXGIAdapter2-4`, `IDXGISwapChain2/4`,
  `IDXGIOutput1-6`, HDR metadata, or `DXGI_ADAPTER_FLAG3`.

### 7b. D3D11 API surface never used
- Deferred contexts / command lists (`CreateDeferredContext`, `ID3D11CommandList`).
- D3D11.1+ device/context interfaces and methods: `ID3D11Device1/2/3`,
  `ID3D11DeviceContext1/2/3`, `ClearView`, `DiscardResource`, `DiscardView`,
  `CopySubresourceRegion1`, `UpdateSubresource1`, `CopyStructureCount`, etc.
- Instancing (`DrawInstanced`, `DrawIndexedInstanced`), indirect (`DispatchIndirect`),
  auto draw (`DrawAuto`), stream output (`SOSetTargets`), tessellation
  (`HSSetShader`, `DSSetShader`).
- Queries other than `D3D11_QUERY_EVENT`: no occlusion, timestamp, pipeline statistics,
  predicates, counters (`CreatePredicate`, `CreateCounter`).
- Shared resources / keyed mutex (`OpenSharedResource`, shared handles).
- Mip generation (`GenerateMips`), resource min LOD, text filter size, `CheckFeatureSupport`
  (beyond the MSAA probe), private-data/name APIs (`SetPrivateData`, `GetPrivateData`).
- Old swap-chain creation (`CreateSwapChain`), CoreWindow swap chains, fullscreen state,
  `ResizeTarget`, frame-latency waitable object / maximum frame latency.
- `CreateTexture1D`, `CreateClassLinkage`, `CreateDeviceContextState`,
  `D3D11CreateDeviceAndSwapChain`, `D3D11On12CreateDevice`.
- `ID3D11Texture2D` copy flags / typed-vs-raw variants beyond those listed in section 3.

### 7c. Outside the inventoried set
- The D3D12 backend (`src/graphics/d3d12/`) and the Vulkan backend
  (`src/graphics/vulkan/`) — separate backends, not D3D11 call sites.
- The DXBC translator internals (`pipeline/shader/dxbc_translator*.cpp`,
  `dxbc_geometry_shaders.cpp`, `dxbc.cpp`) and the shader-pack / DXC-to-DXIL / 29 missing
  boot shaders track. The translator is a producer feeding `Create*Shader`; its internals
  are not part of the call inventory.
- Phase 13 diagnostics and state (`fh1_debug_resolve_alias_*`, resolve probes, notes) —
  read-only here; no Phase 13 file or default is touched.
- Unit tests (`tests/unit/CMakeLists.txt` references the backend) and
  `thirdparty/sdl3/src/render/direct3d11` (SDL's own renderer, not Pinyon's backend).
- Window creation, input, audio, and OS services (SDL3 and platform code).
- Build scripts and `.musebak` backups.

## 8. Unclassified call sites

None of the 75 backend entry points is unclassified. The raw `->Method(` scan also found
79 names that are the project's own methods on project objects; they are deliberately not
in the inventory. Examples: `FindSurface`, `GetOrCreateSurface`, `Initialize`, `Shutdown`,
`ReadbackFromHostBuffer`, `RequestRange`, `CopyReadbackToGuest`, `AnalyzeShaderUcode`,
`BindPipeline`, `ConfigurePipeline`, `ResolveSourceOwner`, `PresentGuestOutputTexture`,
`LogStats`. They appear on `native_surfaces_`, `shared_memory_`, `upload_sink`,
`pipeline_cache_`, `texture_cache_`, `primitive_processor_`, `native_executor_`,
`native_resolve_`, `presenter`, etc.

Count notes:
- `GetImmediateContext`: 39 name matches, 1 API call (38 are the provider accessor).
- `Draw`: 9 matches, 7 calls (definition + header declaration excluded).
- Chained receivers resolved by hand and counted: `GetD3D11Provider().GetDevice()->
  CreateSamplerState/CreateGeometryShader/CreateQuery`; `paint_context.swap_chain->
  GetBuffer/GetDesc1/GetCurrentBackBufferIndex/Present/ResizeBuffers`;
  `provider->GetImmediateContext()->ClearRenderTargetView/Flush/OMSetRenderTargets`.
- The D3D11 UI presenter's `Draw` calls and its device/context creation are included; its
  `D3D11Provider::GetImmediateContext()` accessor calls are not API calls.

## 9. Read-only verification

- Start of Phase 0 work: `2026-10-26T02:13:29Z` (local 2026-10-25 20:13:29).
- Check run:
  `find /f/PinyonShift/work/.local/rexglue/src /f/PinyonShift/work/.local/rexglue/include
  /f/PinyonShift/work/tools /f/PinyonShift/work/config -type f -newermt "@1792980809"`
  → **0 files**.
  The two files edited in the immediately preceding DX11 phase
  (`src/graphics/d3d11/command_processor.cpp`, `.local/DX11_FINISH_NOTES.md`) have mtimes
  2026-10-25 19:55:00 and 20:05:24 local, both before the start timestamp.
- All Phase 0 operations were reads (`ls`, `find`, `grep`, `read_files`) plus the creation
  of this file under `F:\PinyonShift\dx12-phase0`. No game run, no build, no default
  change, no write under `F:\PinyonShift\work`.

## 10. What remains for Phase 1

- Define slices from the six groups above (candidate seams: device/swap-chain/present;
  mirror + upload; EDRAM surfaces + resolve; texture cache; shader/pipeline; draw path;
  shared UI tick).
- For each slice, pin the ordering and lifetime contract a DX12-style equivalent must
  preserve: single immediate-context order, OM-unbind before compute SRV,
  resolve-staging→mirror copy, page-valid tracking, back-buffer acquire/present cadence.
- Decide the treatment of FL 11_0 constraints that the DX12-style path must emulate:
  no sparse/tiled resources, no large-offset raw UAV stores (staging copy), no D3D11.1+
  methods, no debug layer, runtime DXBC translation (no prebuilt pack).
- Decide how the shared UI DXGI tick (M11) and the DXBC translator producer are owned
  across slices.
- Do not write translation code or flip defaults until Phase 1 slices are agreed.
