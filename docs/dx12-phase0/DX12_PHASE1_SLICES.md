# DX12 Phase 1 — Slice Definition and Contracts

Status: Phase 1 only. Slices and contracts. No translation code, no wrappers, no default
changes. Phase 13 and the shader-pack track are untouched.
Generated: 2026-10-26 (UTC), from the Phase 0 inventory and a read-only pass over the
live tree.
Input: `F:\PinyonShift\dx12-phase0\DX12_PHASE0_INVENTORY.md` (426 lines, 75 backend API
entry points / 394 call sites, plus 4 adjacent UI DXGI entry points / 6 sites).
Output folder: `F:\PinyonShift\dx12-phase0`.

## 0. What a slice is here

A slice is one stateful mechanism from Phase 0 section 6, plus the inventoried entry
points that it alone issues, with a boundary a DX12-style equivalent can be cut along.

A boundary qualifies as a slice boundary only if all three hold:
1. The slice owns state with a lifetime that can be stated without naming another slice's
   internals (a resource, a cache, a queue position).
2. Its calls can be replayed from its own recorded state: no call depends on a hidden
   side effect of another slice beyond the handoffs listed here.
3. Every ordering edge crossing the boundary is expressible as submission order on the
   single stream, a resource transition, or a wait on the one frame fence.

Two pieces are deliberately **not** slices:
- **U1, the shared UI DXGI vblank tick.** It is used by every backend and must not be
  wrapped as a backend-specific mechanism (Phase 0 M11).
- **P1, the DXBC translator.** It is a producer feeding shader creation; it makes no
  D3D11 calls and is outside the wrapper boundary (Phase 0 section 7c, M6).

Entry-point attribution rule: each of the 75 backend entry points has exactly one
**primary** slice. The site counts below are attribution, not a partition of usage:
several entry points are issued by more than one slice, and those shared uses are listed
in section 2. The primary counts sum exactly to the Phase 0 total (394).

## 1. Slice coverage

| Slice | Name | Phase 0 mechanisms | Primary entry points | Primary sites |
|---|---|---|---:|---:|
| S1 | Device, swap chain, present, teardown | M8, M9 (use) | 16 | 87 |
| S2 | Guest-memory mirror, transfers, fence | M4, M9 | 7 | 69 |
| S3 | EDRAM surfaces and host format policy | M1, M10 | 7 | 46 |
| S4 | Resolve and copy-out | M2, M3 | 7 | 18 |
| S5 | Tiled texture cache and swap upload | M5 | 6 | 18 |
| S6 | Shader translation and pipeline state | M6 | 24 | 110 |
| S7 | Guest draw and primitive submission | M7 | 8 | 46 |
| | **Backend total** | | **75** | **394** |
| U1 | Shared UI DXGI vblank tick (seam, not a slice) | M11 | +4 new | +6 |
| | **Combined** | | **79** | **400** |

Entry points by slice:

- **S1 (16):** `D3D11CreateDevice`, `CreateDXGIFactory1`, `QueryInterface`,
  `EnumAdapters1`, `GetDesc1`, `CreateSwapChainForHwnd`, `MakeWindowAssociation`,
  `GetImmediateContext`, `ClearState`, `Flush`, `GetCurrentBackBufferIndex`, `GetBuffer`,
  `Present`, `ResizeBuffers`, `Release`, `GetDeviceRemovedReason`.
- **S2 (7):** `CreateBuffer`, `Map`, `Unmap`, `CopySubresourceRegion`, `CreateQuery`,
  `End`, `GetData`.
- **S3 (7):** `CreateTexture2D`, `CreateRenderTargetView`, `CreateDepthStencilView`,
  `ClearRenderTargetView`, `ClearDepthStencilView`, `GetDesc`,
  `CheckMultisampleQualityLevels`.
- **S4 (7):** `CreateUnorderedAccessView`, `CopyResource`, `ResolveSubresource`,
  `Dispatch`, `CSGetShaderResources`, `CSGetConstantBuffers`,
  `CSGetUnorderedAccessViews`.
- **S5 (6):** `CreateTexture3D`, `CreateShaderResourceView`, `GetResource`,
  `UpdateSubresource`, `CreateSamplerState`, `AddRef`.
- **S6 (24):** the 25 shader/pipeline entry points from Phase 0 section 4, less
  `CreateSamplerState` (S5).
- **S7 (8):** `Draw`, `DrawIndexed`, `IASetPrimitiveTopology`, `IASetVertexBuffers`,
  `IASetIndexBuffer`, `RSSetViewports`, `RSSetScissorRects`, `OMSetRenderTargets`.
- **U1 (4 new):** `EnumAdapters`, `IsCurrent`, `EnumOutputs`, `WaitForVBlank`
  (`CreateDXGIFactory1` and `GetDesc` are already counted in S1/S3).

## 2. Shared entry points and how their sites were attributed

| Entry point | Primary | Also issued by | Sites | Why it is shared |
|---|---|---|---:|---|
| `CreateBuffer` | S2 | S6 | 16 | Mirror, 4 MiB upload staging, readback and probe buffers; also the five tracked constant-buffer slots. |
| `Map` / `Unmap` | S2 | S5, S6 | 20 / 22 | Mirror staging writes, texture-cache upload staging, constant-buffer `WRITE_DISCARD`. |
| `CopySubresourceRegion` | S2 | S4, S5 | 8 | Staging→mirror chunks, resolve staging→mirror, texture staging→texture. |
| `CopyResource` | S4 | S2 | 5 | Whole-surface copies for readback/diagnostics, including the MSAA readback path. |
| `CreateTexture2D` | S3 | S4, S5, S1 | 14 | EDRAM surfaces, resolve/readback staging, cache textures, UI images. |
| `CreateShaderResourceView` | S5 | S3, S4 | 9 | Cache SRVs, EDRAM surface SRVs, one-use resolve source views. |
| `CreateUnorderedAccessView` | S4 | S2 | 5 | Resolve destinations, whole-mirror raw view, probe/window views. |
| `GetDesc` | S3 | S1, S4, S5, U1 | 18 | Texture/resource/output descriptions before copies, dumps and swap-chain checks. |
| `UpdateSubresource` | S5 | S6 | 3 | Untiled texture upload; resolve constant-buffer update mode. |
| `ClearRenderTargetView` | S3 | S1 | 4 | Surface-creation clears and back-buffer clear. |
| `Dispatch` | S4 | S6 | 3 | One resolve dispatch; two word-transfer dispatches issued by the native executor. |
| `Flush` | S1 | S2, S4 | 4 | Before waits and present; never a substitute for the fence. |
| `Release` | S1 | all | 61 | Lifetime bookkeeping for every interface; not ownership. |

Consequence for Phase 2: wrapper boundaries cannot follow entry points alone. A wrapper
must be cut by **caller and mechanism**, e.g. `CreateBuffer` calls from the mirror service
belong to S2 while calls from the constant-buffer allocator belong to S6.

## 3. The frame spine

The slices are not independent; this is the order the current backend relies on, and the
order a DX12-style executor must preserve:

1. Guest draws render into EDRAM surfaces (S7 with S6 state and S3 targets).
2. A copy packet is planned and its source owner chosen (S4).
3. Output-merger targets are unbound (S4).
4. One compute resolve dispatch per source surface; the destination is the resolve
   staging buffer on the default path (S4).
5. The staging extent is copied into the 512 MiB mirror at its guest address (S4 → S2).
6. The resolved range is marked GPU-written (S4 → S2 page state, S5 cache invalidation).
7. At the swap: one GPU wait drains every resolve of the frame (S2).
8. If enabled, the front buffer's fetch extent is read back from the mirror and copied to
   guest memory (S4 → S2).
9. The swap texture is invalidated CPU-valid and untiling-uploaded from guest memory (S5).
10. The presenter blits the guest output and runs UI drawers into the back buffer (S1),
    then presents (S1), on the U1 vblank cadence.

Steps 8 and 9's refresh are flag-gated (`fh1_d3d11_resolve_readback`,
`fh1_resolve_guest_refresh`). This phase does not flip them or require them: the contract
states the ordering that must hold **when** a refresh path is enabled. With both off, S5
reads guest memory as-is, which is the current default and is not changed here.

Evidence anchors: `command_processor.cpp:2949` (staging→mirror copy), `:3043`
(`MarkRangeAsResolved`), `:3088` and `:3113-3130` (`WaitForGpu`), `:3220-3264` (swap wait
and front-buffer readback), `texture_cache.cpp:709-782` (swap extent, invalidate, upload),
`fh1_native_resolve.cpp:226` (OM unbind), `:339` (dispatch),
`d3d11_presenter.cpp:330-365` and `:505` (acquire, clear, present).

## 4. Slices

### S1 — Device, swap chain, present, teardown

**Owns.** The single device and immediate context, adapter selection, the presenter swap
chain and its per-buffer render-target views, resize, present, device-loss reporting, and
all COM lifetime bookkeeping. Mechanisms M8 and the consumer side of M9.

**Ordering contract.**
1. Device and immediate context are created before any resource, shader, query or swap
   chain. The adapter is selected by description substring (`d3d11_adapter`, live target
   `GTX 570`), never by index; the created feature level must be exactly 11_0, otherwise
   initialization fails.
2. All slices record into the one immediate stream, and execution order is submission
   order. A slice boundary may not introduce parallel recording; the backend has no
   deferred contexts and no second queue.
3. The swap-chain tier is chosen at creation only, in the order FLIP_DISCARD (3 buffers)
   → FLIP_SEQUENTIAL (3) → blt (1). A tier is accepted only when creation **and** first
   buffer acquisition both succeed, not on creation alone.
4. Per paint: read the current back-buffer index → take that index's existing RTV →
   clear → blit the guest output if the mailbox has an active image → run UI drawers →
   `Present(vsync)`. `Present` is never called with an RTV acquired before a resize.
5. Resize: drain the GPU through the S2 fence, release every per-buffer RTV and every
   reference into a swap-chain buffer, `ResizeBuffers`, recreate the RTVs, then resume
   painting. No backend resource may alias a swap-chain buffer at any time.
6. Device removal/reset observed at `Present` or at a failed create/copy is reported once
   through the host-GPU-loss callback; no further commands are submitted.
7. `Present` runs on the thread that owns the window; the U1 vblank wait never runs on
   that thread and never touches the device.

**Lifetime contract.**
1. Device and context are process-lifetime, provider-owned, and outlive every resource
   created by S2–S7.
2. Factory outlives swap chain; swap chain outlives its buffers; each back-buffer RTV is
   owned per buffer index and released before resize or teardown. Back-buffer textures are
   never cached beyond the frame in which they were acquired.
3. The presenter owns the guest-output images, their textures and SRVs; the backend only
   supplies a source SRV for the duration of the blit call and retains no reference.
4. Teardown order: stop painting and UI → release back-buffer RTVs → release swap chain →
   release presenter images → shut down backend caches (S6, S5, S3) → release mirror and
   its views (S2) → `ClearState` → release context → factory → device.
5. No S1 state holds a borrowed pointer into S3/S5 across a frame boundary.

**Handoffs.** S1 consumes S2's fence before resize/teardown, S3's UI/guest-output textures
for the blit, and S5's swap SRV for one call. It provides the device/context to every
slice.

**Contract checks.** Exactly one `Present` per painted frame; no present after a failed
resize; the presented RTV index equals the acquired index; no swap-chain buffer appears as
an SRV or UAV anywhere in the frame.

---

### S2 — Guest-memory mirror, transfers, fence

**Owns.** The 512 MiB committed mirror, the 4 MiB upload staging buffer, the readback
staging buffer, chunked CPU↔mirror transfers, raw mirror views, page-valid/GPU-written
state, and the single event-query fence. Mechanisms M4 and M9.

**Ordering contract.**
1. CPU→mirror upload: mark the pages valid **before** the staging copy, so a CPU write
   racing the copy clears validity and forces re-upload (`shared_memory.cpp:62-80`); then
   `Map(WRITE)` → `memcpy` → `Unmap` → `CopySubresourceRegion` in ascending chunks of at
   most 4 MiB.
2. GPU→mirror: the S4 resolve dispatch precedes the staging→mirror copy on the same
   stream. No CPU read of the extent may be recorded between them.
3. Mirror→CPU: the fence must have completed after all GPU writes to the range, and the
   readback staging must not be mapped while a copy into it is queued.
4. Page ownership: a CPU upload must not overwrite a GPU-written page; a resolve marks its
   extent GPU-written after dispatch and copy; a readback-to-guest clears GPU-written for
   the copied pages so the next CPU upload may proceed.
5. Fence: one `D3D11_QUERY_EVENT`, created lazily, `End` recorded after the last
   submission that must be observed, completion polled with `GetData`. There is no other
   fence type, no per-resource fence, and no submission thread.

**Lifetime contract.**
1. The mirror is created before any view over it and destroyed after every such view; it
   is committed for the process lifetime at feature level 11_0 (no sparse/tiled
   resources).
2. The 4 MiB upload staging is a single resource, serialized and never mapped twice; the
   readback staging is separate and never written by the GPU.
3. Raw whole-buffer views are cached for the mirror's lifetime; one-use resolve views are
   owned by S4, not by S2.
4. The page bitmap is owned by the mirror, in units of `SharedMemory::page_size_log2()`,
   and is reset only by cache clear or teardown; GPU-written state survives across frames
   until consumed.
5. The resolve staging buffer (cap 64 MiB) belongs to the upload sink and is reused; probe
   and mirror-window targets are diagnostic and never coexist with it on the production
   path.

**Handoffs.** S2 consumes S4's dispatch/copy order and S5's page-state questions; it
provides the fence to S1, S4 and S5, and raw buffer views to S4/S7.

**Contract checks.** Zero CPU reads of a range with queued GPU writes; every byte the
texture cache reads is either CPU-valid or copied back from the mirror; every `Map(READ)`
follows a completed fence; upload chunks never exceed 4 MiB and never skip a page.

---

### S3 — EDRAM surfaces and host format policy

**Owns.** The EDRAM surface registry, surface texture/view creation, creation clears, the
host format capability tables, MSAA sample-count probing, and resource/view descriptions.
Mechanisms M1 and M10.

**Ordering contract.**
1. Resolve the surface key before lookup: base tiles | pitch tiles | sample count | depth
   flag | storage format, with height derived from pitch and clamped by the guest/host
   limits. A miss creates the surface and clears it before returning it for use.
2. The creation clear is submitted before the first draw or resolve that reads the
   surface. There is no path that binds a newly created surface without its clear.
3. Texture → RTV/DSV → SRV creation order; the sample count and draw scale are fixed at
   creation from host capability, never inferred per draw.
4. A surface is either an output-merger target or a shader resource, never both at once.
   S4 unbinds OM before sampling; S7 rebinds its targets before the next draw.
5. Depth surfaces share one texture across DSV, depth SRV and optional stencil SRV; the
   resolve key and owner key may differ, and the owner key names the source format.
6. Draw-view format may differ from the storage resource format (typed resource vs UNORM
   view); the choice is made at creation.

**Lifetime contract.**
1. The registry is a process-lifetime map keyed by packed key with no eviction; it is
   cleared only at `Shutdown` (`fh1_native_surfaces.cpp:51-61`). Surface count is monotonic
   within a session.
2. A surface owns its texture and up to five views; views are released before the texture,
   and the texture before the device.
3. No draw or resolve destroys a surface on behalf of another slice; users hold borrowed
   pointers valid until `Shutdown`.
4. Registry shutdown happens after the last draw/resolve and after the S2 drain.
5. MSAA sample count and draw resolution scale are creation-time properties; a change is a
   new key, not a mutation.

**Handoffs.** S3 provides surfaces and views to S4 and S7, and format answers to S6; it
consumes S2's fence only at shutdown.

**Contract checks.** Every resolve source has a registry entry; no surface is created
during a resolve dispatch; no view outlives its surface; the creation clear precedes the
first read for every new surface.

---

### S4 — Resolve and copy-out

**Owns.** Copy-packet planning, source-owner selection, the compute resolve dispatch, the
staging→mirror copy, MSAA readback resolve, resolve diagnostics, and the resolve's
constant data. Mechanisms M2 and M3, with M9's fence as a service.

**Ordering contract.**
1. Plan the copy from the guest registers first; an empty plan does no work and a skipped
   resolve leaves all state untouched.
2. Choose the source owner surface by key and rectangle before any binding.
3. Unbind the output merger before binding source SRVs. This is explicit: D3D11 silently
   drops an SRV still bound as an RTV/DSV, and the DX12-style equivalent must make the
   transition explicit rather than rely on implicit unbinding
   (`fh1_native_resolve.cpp:207-231`).
4. Bind order: compute shader → constant buffer → source SRV(s) → destination UAV →
   `Dispatch(ceil(w/8), ceil(h/8), 1)`.
5. After the dispatch: unbind UAV, SRVs and shader, then release the one-use views.
6. Default production path: staging extent → mirror at the guest address, then mark the
   range GPU-written, then record the last resolve for the swap trace.
7. A diagnostic readback may be inserted only after its own fence and must not reorder the
   production copy. Diagnostics never become the source of truth.
8. `ResolveSubresource` (MSAA readback) is a separate ordered operation before the map
   that reads its destination.
9. The frame readback, when enabled, runs after the swap fence and copies exactly the
   fetch extent from mirror to guest; it is not per-resolve.

**Lifetime contract.**
1. Resolve compute pipelines are cached per source kind × MSAA for the backend lifetime.
2. The resolve constant buffer is persistent and updated per dispatch
   (`Map(WRITE_DISCARD)` or `UpdateSubresource`); it is never allocated per resolve.
3. One-use SRVs/UAVs are created and released inside the resolve; no reference survives
   the dispatch.
4. The 64 MiB resolve staging buffer is reused. Direct-mirror, mirror-window and small-UAV
   probe targets are exclusive diagnostic alternatives, never coexisting production
   targets.
5. The fence belongs to S2; S4 never creates a second one.
6. Readback scratch is CPU memory reused across resolves; it has no GPU lifetime.

**Handoffs.** S4 consumes S3's source surface, S2's mirror/staging/fence and S6's compute
pipeline; it provides S2's page marks and S5's guest bytes.

**Contract checks.** One dispatch per resolve; OM empty at dispatch; the bytes copied to
the mirror equal the staging bytes; one resolve writes one guest extent; the frame
readback range equals the fetch extent and never exceeds the mirror.
Known asymmetry carried forward, not changed here: the destination's `copy_dest_swap` is
applied while the swap fetch's swizzle (`0x60A`) is dropped by S5, so correct rows arrive
R/B-swapped. Phase 13 records it; Phase 1 neither fixes nor hides it.

---

### S5 — Tiled texture cache and swap upload

**Owns.** Guest fetch → texture key, host texture creation, CPU untiling and upload, SRV
variants, sampler cache, LRU and guest-base lookup, and the swap texture upload. Mechanism
M5.

**Ordering contract.**
1. Decode the fetch to a texture key (base page, format, tiling, dimensions, mip count,
   endianness, swizzle) before lookup. An invalid key returns null and uploads nothing.
2. `FindOrCreateTexture` lays out the host texture only; pixels exist only after
   `LoadTextureData`. A cache hit is not evidence of content.
3. Before a CPU untiling read, check page state: a GPU-written range requires the S4/S2
   refresh to land bytes in guest memory first; a refused load leaves the previous host
   content in place and logs (`texture_cache.cpp:560-600`).
4. CPU untile into a linear staging image (256-byte row pitch), then `UpdateSubresource`
   per mip/slice, then bind the SRV. An upload is not split across frames.
5. Swap upload: read fetch 0's extent → optional front-buffer readback (S4) → invalidate
   the span as CPU-valid (`texture_cache.cpp:769`) → `LoadTextureData` (`:771`) → return
   the SRV. The upload must observe post-resolve guest bytes when a refresh path is
   enabled.
6. Touch the LRU only after a successful use; eviction happens between commands, never
   while a texture is bound in the current draw or resolve.
7. Sampler state is created/cached from the fetch before the draw that binds it.

**Lifetime contract.**
1. The cache owns host textures and evicts by last-use frame; a returned texture pointer is
   valid only until eviction (documented in the backend header).
2. A texture's SRV `AddRef` keeps the resource alive for the draw that binds it; the draw
   releases it afterwards.
3. Variant SRVs (unsigned/signed, stacked-as-2D, cube) are owned by the texture and
   released with it.
4. The swap texture is a normal cache entry; it is not pinned across frames and is not
   exempt from LRU.
5. The cache is shut down after the last draw/resolve and the S2 drain, before the device.
6. There is no mip generation; every mip is uploaded explicitly.

**Handoffs.** S5 consumes S2's page state and guest bytes, S4's resolve output, and S3's
format policy; it provides the swap SRV to S1 and texture SRVs to S6/S7.

**Contract checks.** After an enabled refresh path, the uploaded swap bytes equal the
guest range; no upload of a GPU-written page; no eviction of a bound texture; every SRV
variant belongs to a live texture.

Known asymmetry (same as S4): the fetch swizzle is dropped here while the resolve applies
its destination swap. Recorded, not addressed in Phase 1.

---

### S6 — Shader translation and pipeline state

**Owns.** Runtime shader translation handoff, stage-object creation, pipeline/state
caches, constant-buffer contents and updates, sampler objects, and the bind sequence.
Mechanism M6, with P1 as its producer.

**Ordering contract.**
1. Guest shader modification → translator key → DXBC → `Create*Shader`, all before the
   first draw that needs the object; the cache is consulted first.
2. Blend/depth/raster state and geometry shaders are looked up or created before binding,
   not allocated on the draw hot path.
3. A constant-buffer update is ordered before the draw/dispatch that reads it, using
   `WRITE_DISCARD` mapping or `UpdateSubresource`; an in-flight buffer's contents are
   never rewritten in place.
4. Per-pass bind order: topology/input → shaders → constant buffers → SRVs → samplers →
   rasterizer viewport/scissor/state → output-merger targets and states → draw/dispatch.
5. The geometry shader exists only for quad-list emulation; there are no hull/domain
   stages, and the input layout is always null.

**Lifetime contract.**
1. Shader, pipeline and geometry-shader objects are owned by the pipeline cache, are
   immutable after creation, and are released only at `Shutdown`
   (`pipeline_cache.cpp:133-143`). They are not released per draw.
2. State objects are shared by key and never mutated after creation.
3. The five tracked constant-buffer slots persist for the backend lifetime and are sized
   at initialization.
4. No shader or state object outlives its cache; the cache is destroyed after the last
   draw and before the device.
5. Translator output is owned by the cache; P1 retains no resource.

**Handoffs.** S6 consumes P1's DXBC and S3's format policy; it provides bound state to S7
and compute pipelines to S4.

**Contract checks.** Every draw's shader and state objects exist before the draw; no null
shader bind; constant-buffer contents correspond to the guest packet at draw time; no
per-draw compile on the hot path.

---

### S7 — Guest draw and primitive submission

**Owns.** Guest draw decoding, primitive processing, host vertex/index buffer selection,
topology/viewport/scissor application, render-target binding for draws, and the
`Draw`/`DrawIndexed` submission. Mechanism M7.

**Ordering contract.**
1. Decode the guest draw → primitive processor produces or selects host vertex/index
   buffers → bindings resolved.
2. Guest-DMA indices bind the mirror directly (S2); converted or builtin indices use host
   buffers that must be fully written before the draw.
3. Bind sequence: shaders/constants/SRVs/samplers (S6) → render targets and depth (S3) →
   viewport/scissor → topology → `Draw` or `DrawIndexed`.
4. A draw into a surface that will be resolved must be submitted before the resolve is
   recorded; the two never interleave in a way that changes the target between them.
5. Transfer passes (`Draw(3)`) share the same ordered stream and obey the same
   target/SRV non-aliasing rule as guest draws.
6. Topology mapping (including quad-list → geometry shader) is decided before the draw;
   no state change is recorded between the draw and its dependent copy.

**Lifetime contract.**
1. Converted/builtin host vertex and index buffers are owned by the primitive processor and
   valid through submission; guest-DMA index data is owned by the mirror, not copied.
2. Draw info and surface pointers are borrowed for the duration of submission and not
   retained past the draw.
3. Render targets and views are owned by S3 and pipeline objects by S6; the draw path
   releases neither.
4. Recording is synchronous: one draw completes recording before the next guest command.

**Handoffs.** S7 consumes S3 targets, S6 state, S2 index data and S5 texture SRVs; it
provides completed draws to S4's resolve.

**Contract checks.** No draw with an unwritten vertex buffer; an indexed draw's range lies
inside its bound buffer; the target surface matches the key of the resolve that later
reads it; no draw is recorded into a surface currently bound as a resolve source.

## 5. Shared seams

### U1 — Shared UI DXGI vblank tick

**Owns.** `CreateDXGIFactory1` (shared use), `IDXGIFactory1::IsCurrent`,
`IDXGIFactory1::EnumAdapters`, `IDXGIAdapter::EnumOutputs`, `IDXGIOutput::GetDesc`,
`IDXGIOutput::WaitForVBlank` — 6 sites, 4 entry points not otherwise in the backend list.

**Ordering contract.** The vblank wait runs on a worker thread; no device or context call
runs on that thread. The factory is recreated when the monitor set changes; `IsCurrent` is
checked before reusing it. Paint happens on the window-owning thread and does not wait for
vblank itself.

**Lifetime contract.** Factory, adapter and output objects are thread-local to the tick and
never stored in a backend cache. They are released when the tick stops, independently of
the device.

**Rule.** U1 is shared by every backend and is reused as-is. It is not wrapped and not
owned by S1, even though S1's paint depends on its cadence.

### P1 — DXBC translator producer

**Contract.** Consumes a guest shader modification and produces DXBC; called from S6
before `Create*Shader`. It makes no D3D11 calls and owns no GPU resource. The shader-pack /
DXC-to-DXIL / 29-missing-boot-shaders track stays separate and is not part of this
boundary.

## 6. Cross-slice invariants

| # | Invariant | Enforced by |
|---|---|---|
| I1 | One device, one immediate stream; execution order equals submission order. | S1 |
| I2 | A resource is never sampled while bound as an output target. | S4, S7 |
| I3 | A resolve dispatch precedes its staging→mirror copy. | S4, S2 |
| I4 | Every CPU read of GPU-written bytes follows the single frame fence. | S2 |
| I5 | CPU uploads never overwrite GPU-written pages; GPU writes mark ownership before readback. | S2 |
| I6 | One surface per key, cleared at creation before first use, never evicted. | S3 |
| I7 | A bound texture is never evicted; an SRV keeps its resource alive for the draw. | S5 |
| I8 | The swap upload reads guest memory only, after the enabled refresh path. | S5 |
| I9 | Shader/state objects are immutable and exist before the draw that binds them. | S6 |
| I10 | Vertex/index data is complete before the draw is recorded. | S7 |
| I11 | Present follows a completed paint of the acquired back buffer. | S1 |

These eleven invariants are the acceptance surface for Phase 2: a DX12-style translation
that preserves them is behaviourally equivalent for this inventory, regardless of how many
command lists or barriers it uses internally.

## 7. What Phase 1 settles, and what it does not

Settled:
- Seven backend slices plus two shared seams cover all 75 inventoried entry points and all
  394 sites; nothing is unowned.
- The ordering edges between slices are enumerated (section 3) and each slice states its
  own must-precede rules.
- Ownership and destruction order is stated per slice; no object has two owners.
- The Phase 0 out-of-scope list still applies: nothing outside this inventory gets a
  DX12-style wrapper, and anything missing is added to the inventory first.

Not settled (Phase 2 decisions):
1. Command-list granularity: one list per frame, one per slice, or one per mechanism. The
   only hard requirement is that the single-stream order in section 3 survives.
2. Resource-state tracking: which slice owns the state table that turns I2/I3 into
   explicit transitions (S3 surfaces, S5 textures, S2 buffers overlap).
3. Whether S1's presenter swap chain stays a D3D11/DXGI surface while only the backend
   becomes DX12-style, or is replaced too.
4. Fence granularity: the current contract permits exactly one frame fence plus diagnostic
   waits; anything finer is a change to S2's contract, not an implementation detail.
5. The fetch-swizzle / destination-swap asymmetry recorded in S4/S5: leave, or fix under
   the shader/texture track.
6. How U1 is shared if the DX12-style path gets its own presenter, and whether the shader
   pack replaces P1 for the DX12 path.

## 8. Out of scope (carried forward from Phase 0)

- Translation or wrapper code of any kind; Phase 1 is a contract document.
- D3D11 features listed in Phase 0 section 7a/7b (D3DCompiler, debug layer, D3D11.1+,
  deferred contexts, instancing/indirect/stream-output/tessellation, non-EVENT queries,
  shared resources, mip generation, old swap-chain APIs).
- The D3D12 and Vulkan backends already in the tree (`src/graphics/d3d12/`,
  `src/graphics/vulkan/`) — they are separate implementations, not call sites in this
  inventory.
- DXBC translator internals and the shader-pack / DXC-to-DXIL / missing-boot-shader track.
- Phase 13's shared-base separation fix, its probes and its flags. This document neither
  touches nor depends on that work.
- Default changes. `fh1_native_executor`, `fh1_d3d11_resolve_readback` and
  `fh1_resolve_guest_refresh` stay as they are; flipping them is Seth's call.

## 9. Read-only verification

- Start of Phase 1 work: 2026-10-26T18:54:00Z.
- Check run after writing this file:
  `find /f/PinyonShift/work/.local/rexglue/src /f/PinyonShift/work/.local/rexglue/include
  /f/PinyonShift/work/tools /f/PinyonShift/work/config -type f -newermt "@1793040840"`
  → **0 files**. The newest backend source mtime remains 2026-10-25 19:55:01, from the
  Phase 13 work that preceded Phase 0.
- All Phase 1 operations were reads (`grep`, `sed`, `read_files`) plus the creation of this
  file under `F:\PinyonShift\dx12-phase0`. No build, no game run, no default change, no
  write under `F:\PinyonShift\work`.
