/**
 * @file        graphics/d3d11/fh1_native_executor.h
 * @brief       D3D11 backend: FH1 native executor (targets, ownership, transfers)
 *
 *              The Direct3D 11 counterpart of the Direct3D 12 Fh1NativeExecutor
 *              (src/graphics/d3d12/fh1_native_executor.cpp): it owns the native
 *              EDRAM surfaces a guest draw writes, keeps their EDRAM tile
 *              ownership (the API-agnostic Fh1EdramTiles, shared with the
 *              Direct3D 12 and Vulkan executors), and binds them in place of
 *              the render target cache's update. The surfaces are the ones the
 *              resolve path reads (one registry: D3D11Fh1NativeSurfaces), so a
 *              draw's pixels and the resolve that copies them out agree on the
 *              texture by construction rather than by coincidence.
 *
 *              Scope (C1 and C2, behind fh1_native_executor):
 *              - PrepareTargets: derives the draw's targets from the guest
 *                registers exactly as the render target path did, claims the
 *                tiles the draw's rectangle covers, and records them for
 *                BindTargets.
 *              - Transfers (C2): a surface that takes tiles another surface
 *                wrote receives their content, the guest seeing the previous
 *                owner's words through the claimant's layout and format. An
 *                identical host format is a CopySubresourceRegion; otherwise
 *                the ownership transfer pixel shaders (the same ones the
 *                Direct3D 12 executor uses, compiled for SM 5.0) draw the
 *                tiles' rectangles, decoding through the source's layout and
 *                encoding through the destination's.
 *              - Depth tail (C3): a depth destination whose source is not the
 *                same host format goes through the EDRAM words pass - a
 *                compute dispatch producing the previous owner's guest words
 *                per destination sample, then the from_words depth pass (which
 *                resets stencil) and one pass per stencil bit. The words
 *                buffer is bound through typed R32_UINT views, one element per
 *                guest EDRAM word: feature level 11_0 has no usable raw view
 *                here (raw stores land only at low offsets, and a sub-range
 *                raw view cannot be created), so no raw view is created at
 *                all.
 *              - Transfer batching (C4): a claim's runs are queued and issued
 *                as one transfer per (previous owner, stencil) pair, so every
 *                rectangle of a destination/source pair shares one compute
 *                dispatch (fh1_native_transfer_words_batch.cs.hlsl) and one
 *                set of depth and stencil passes instead of one call each.
 *              - BindTargets: binds those surfaces (RTVs/DSV) and reports the
 *                bound bits and host formats the pipeline key needs.
 *              - ResolveSourceOwner: the copy path reads the surface the draws
 *                wrote even when its key differs (a storage format, or a format
 *                switch between passes on the same EDRAM).
 *              - NativeDrawIssued / OnSwap / LogStats: accounting.
 *              Capture is EDRAM-only (no guest render target cache), scale 1
 *              only, and the guest-memory mirror is written by nothing here:
 *              the resolve path's staging buffer remains the only write route
 *              (Part 1's contract).
 *
 *              Still open: per-owner rectangle splitting (SplitByOwner, for
 *              copy rectangles that mix owners), the resolve readback default,
 *              and scaled targets.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tom@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <wrl/client.h>

#include <rex/graphics/d3d11/fh1_native_surfaces.h>
#include <rex/graphics/fh1_depth_overwrite.h>
#include <rex/graphics/fh1_edram_surfaces.h>
#include <rex/graphics/fh1_edram_tiles.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/d3d11/d3d11_api.h>

namespace rex::graphics {
class RegisterFile;
}  // namespace rex::graphics

namespace rex::graphics::d3d11 {

class D3D11CommandProcessor;

// Host configuration the executor shares with the surfaces and pipelines it
// reuses.
struct D3D11Fh1NativeExecutorConfig {
  // Color format derivation (D3D11HostRenderConfig::gamma_render_target_as_unorm16).
  bool gamma_as_unorm16 = true;
  // Fixed-point 16_16[_16_16] hosts hold the guest value divided by 32.
  bool fixed16_full_range = false;
  // Resolution scale. The executor executes scale 1 only: Initialize refuses
  // anything else with a logged reason rather than drawing into the wrong-sized
  // surfaces.
  uint32_t scale = 1;
  // The command processor's native surface registry - the same one the resolve
  // path reads.
  D3D11Fh1NativeSurfaces* surfaces = nullptr;
};

class D3D11Fh1NativeExecutor {
 public:
  D3D11Fh1NativeExecutor(D3D11CommandProcessor& command_processor,
                         const RegisterFile& register_file);
  ~D3D11Fh1NativeExecutor();

  D3D11Fh1NativeExecutor(const D3D11Fh1NativeExecutor&) = delete;
  D3D11Fh1NativeExecutor& operator=(const D3D11Fh1NativeExecutor&) = delete;

  bool Initialize(const D3D11Fh1NativeExecutorConfig& config);
  void Shutdown();

  // Before the targets are bound for a draw: derives the surfaces the draw
  // writes from the guest registers, claims the tiles its rectangle covers, and
  // transfers any of them that another surface wrote into the claimant before
  // the claim moves them (C2). `claim_x0`..`claim_y1` is the draw's rectangle
  // in guest pixels (its scissor, an upper bound of what it can write), so
  // claims and transfers stay proportional to the draw rather than to the
  // surface.
  void PrepareTargets(const Fh1DrawInfo& draw, int32_t claim_x0, int32_t claim_y0,
                      int32_t claim_x1, int32_t claim_y1);

  // Binds the surfaces PrepareTargets derived in place of the render target
  // cache's update, and reports the bound slots and their guest formats the
  // way RenderTargetCache::GetLastUpdateBoundRenderTargets did. False skips the
  // draw (nothing to bind).
  bool BindTargets(uint32_t& bound_depth_and_color_render_target_bits_out,
                   uint32_t* bound_depth_and_color_render_target_formats_out);

  // The surface that holds the words under a copy's rectangle, from the tiles
  // the draws claimed: the packed key of their single owner, or
  // Fh1EdramTiles::kNoOwner when the rectangle mixes owners (the caller then
  // reads the surface its own key names). Tiles nobody claimed since Reset are
  // not owners - nothing to inherit - and do not disqualify a rectangle. The
  // format is not part of the decision: the EDRAM words are the same words,
  // and the resolve shader reads them through the copy's format with the layout
  // of the owner that wrote them (its two layout words).
  uint32_t ResolveSourceOwner(const Fh1SurfaceKey& key, int32_t x0, int32_t y0, int32_t x1,
                              int32_t y1) const;

  // After the command processor issued the draw.
  void NativeDrawIssued(const Fh1DrawInfo& draw);

  // After a resolve read its source through ResolveSourceOwner.
  void OwnerResolveIssued() { ++owner_resolves_; }

  // End of a guest frame.
  void OnSwap(uint64_t frame, uint32_t frontbuffer_address);

  // Periodic and shutdown statistics (fh1_native_executor on).
  void LogStats(uint64_t frame);

  const Fh1EdramTiles& tiles() const { return tiles_; }
  bool initialized() const { return initialized_; }
  uint32_t pending_used_bits() const { return pending_used_bits_; }
  const Fh1SurfaceKey& pending_key(uint32_t slot) const { return pending_keys_[slot]; }

 private:
  using SurfaceKey = Fh1SurfaceKey;

  // Claims the tiles of the pixel rectangle for the surface `key`, transferring
  // whatever another surface wrote into it first. `stencil`: the draw may leave
  // a nonzero stencil in those tiles (depth targets with stencil testing).
  void ClaimTargetTiles(const SurfaceKey& key, D3D11Fh1NativeSurface& surface, int32_t x0,
                        int32_t y0, int32_t x1, int32_t y1, bool stencil);

  // Queues one run of EDRAM tiles (which may wrap) of the previous owner for
  // the transfer into `dest`: ClaimTargetTiles collects every run of a claim
  // and FlushClaimTransfers issues them, one batch per (previous owner,
  // stencil) pair, so one compute dispatch carries all of a pair's rectangles.
  void QueueTransferRun(const SurfaceKey& dest_key, D3D11Fh1NativeSurface& dest,
                        uint32_t previous_owner, uint32_t first_tile, uint32_t tile_count);
  // Transfers what the claim queued. Batched, every rectangle of one
  // destination/source pair goes to one TransferRects call; with
  // fh1_debug_transfer_unbatched, one call per run (the C3 behaviour).
  void FlushClaimTransfers(const SurfaceKey& dest_key, D3D11Fh1NativeSurface& dest);
  struct QueuedChunk {
    uint32_t previous_owner = 0;
    bool stencil = false;
    uint32_t tile_count = 0;
    uint32_t rect_first = 0;
    uint32_t rect_count = 0;
  };
  // Transfers the rectangles of the run. Same host format, geometry and sample
  // count: CopySubresourceRegion; otherwise the transfer shader for the
  // destination and source kinds. Anything else is counted and skipped.
  void TransferRects(const SurfaceKey& dest_key, D3D11Fh1NativeSurface& dest,
                     uint32_t previous_owner, const RECT* rects, uint32_t rect_count,
                     uint32_t tile_count, bool stencil);

  // A depth destination's tiles, through the EDRAM words pass: the previous
  // owner's words are computed once per destination sample by a compute
  // dispatch (one per destination/source pair, up to 32 rectangles each), then
  // written into the destination's depth (one pass, which
  // also resets stencil) and, when the source may hold a nonzero stencil, one
  // pass per stencil bit. Direct instead runs the same passes straight from the
  // source, with no words buffer and no compute dispatch at all.
  void TransferDepthRects(const SurfaceKey& dest_key, D3D11Fh1NativeSurface& dest,
                          D3D11Fh1NativeSurface& source, const RECT* rects,
                          uint32_t rect_count, uint32_t tile_count, bool stencil);
  // Whether the direct (dispatch-free) depth path reproduces the words pass for
  // this source: it keeps the default 32x snorm scale, which the words pass
  // takes from the fixed16 flag, so a 16_16 source under the truncating scale
  // stays on the words path.
  bool TransferDirectSupported(const D3D11Fh1NativeSurface& source) const;

  bool EnsureTransferWords(uint64_t word_count);
  ID3D11ComputeShader* GetTransferWordsShader(uint32_t source_kind, bool source_msaa);
  ID3D11ComputeShader* GetTransferWordsBatchShader(uint32_t source_kind, bool source_msaa);
  ID3D11PixelShader* GetTransferFromWordsShader(uint32_t dest_kind, bool dest_msaa);
  ID3D11PixelShader* GetTransferDirectShader(uint32_t source_kind, bool source_msaa,
                                             uint32_t dest_kind, bool dest_msaa);
  ID3D11DepthStencilState* GetTransferDepthPassState(bool reset_stencil);
  ID3D11DepthStencilState* GetTransferStencilState(uint32_t bit);

  // fh1_swap_fetch_trace: reads the same rectangle of the claimant and its
  // predecessor back and logs how much of each is non-zero (the first few
  // transfers of a session).
  int64_t CountZeroTexels(ID3D11Texture2D* texture, const RECT& rect);
  void VerifyTransfer(D3D11Fh1NativeSurface& dest, D3D11Fh1NativeSurface& source,
                      const RECT& rect, uint64_t dest_before);
  // The same, for a depth surface: Direct3D 11 cannot stage-copy a
  // depth-stencil resource, so the rectangle's depth is read through a shader
  // resource view into an R32_FLOAT target and the target is read back.
  int64_t CountEmptyDepth(const D3D11Fh1NativeSurface& surface, const RECT& rect);
  void VerifyDepthTransfer(D3D11Fh1NativeSurface& dest, D3D11Fh1NativeSurface& source,
                           const RECT& rect, uint64_t dest_before);

  bool EnsureTransferResources();
  ID3D11PixelShader* GetTransferShader(uint32_t source_kind, bool source_msaa,
                                       uint32_t dest_kind, bool dest_msaa);

  SurfaceKey MakeColorKey(uint32_t base, uint32_t pitch_tiles, uint32_t msaa,
                          xenos::ColorRenderTargetFormat format) const;
  static SurfaceKey MakeDepthKey(uint32_t base, uint32_t pitch_tiles, uint32_t msaa,
                                 xenos::DepthRenderTargetFormat format);
  static uint32_t PitchTiles(uint32_t pitch_pixels, uint32_t msaa);
  uint32_t SurfaceHeight(uint32_t pitch_tiles, uint32_t msaa) const;

  D3D11CommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  D3D11Fh1NativeExecutorConfig config_;
  // EDRAM tile -> packed key of the surface that last wrote it.
  Fh1EdramTiles tiles_;

  bool initialized_ = false;
  // Targets PrepareTargets derived for the draw BindTargets binds.
  bool pending_targets_valid_ = false;
  uint32_t pending_used_bits_ = 0;
  SurfaceKey pending_keys_[1 + xenos::kMaxColorRenderTargets];
  D3D11Fh1NativeSurface* pending_surfaces_[1 + xenos::kMaxColorRenderTargets] = {};

  // Transfer resources (created on the first transfer that needs them).
  ID3D11VertexShader* transfer_vertex_shader_ = nullptr;
  ID3D11Buffer* transfer_constants_ = nullptr;
  ID3D11RasterizerState* transfer_rasterizer_state_ = nullptr;
  ID3D11DepthStencilState* transfer_depth_stencil_state_ = nullptr;
  std::unordered_map<uint32_t, ID3D11PixelShader*> transfer_pixel_shaders_;

  // Depth destinations (C3): the guest EDRAM words of the previous owner in a
  // typed R32_UINT buffer, one element per word, plus the passes that write
  // them into a depth-stencil surface.
  ID3D11Buffer* transfer_words_buffer_ = nullptr;
  ID3D11UnorderedAccessView* transfer_words_uav_ = nullptr;
  ID3D11ShaderResourceView* transfer_words_srv_ = nullptr;
  uint32_t transfer_words_capacity_ = 0;  // Words the buffer holds.
  std::unordered_map<uint32_t, ID3D11ComputeShader*> transfer_words_shaders_;
  std::unordered_map<uint32_t, ID3D11ComputeShader*> transfer_words_batch_shaders_;
  std::unordered_map<uint32_t, ID3D11PixelShader*> transfer_from_words_shaders_;
  std::unordered_map<uint32_t, ID3D11PixelShader*> transfer_direct_shaders_;
  // The runs of the claim being transferred, and their rectangles: nothing is
  // issued until the whole claim is known, so a pair batches into one dispatch.
  std::vector<RECT> queued_rects_;
  std::vector<QueuedChunk> queued_chunks_;
  // Keyed by the stencil write mask: 0/1 for the depth pass, 0x100 | bit
  // otherwise.
  std::unordered_map<uint32_t, ID3D11DepthStencilState*> transfer_depth_states_;
  ID3D11PixelShader* depth_probe_shader_ = nullptr;
  ID3D11PixelShader* depth_probe_shader_ms_ = nullptr;

  uint64_t draws_ = 0;
  uint64_t prepares_ = 0;
  uint64_t binds_ = 0;
  uint64_t skipped_draws_ = 0;
  uint64_t claims_ = 0;
  uint64_t claimed_tiles_ = 0;
  // Transfers another surface's tiles into a claimant.
  uint64_t transfers_ = 0;
  uint64_t transfer_tiles_ = 0;
  uint64_t transfer_draws_ = 0;
  uint64_t transfer_copies_ = 0;
  uint64_t transfer_skipped_source_ = 0;
  uint64_t transfer_skipped_format_ = 0;
  // Depth destinations through the words pass.
  uint64_t transfer_words_dispatches_ = 0;
  uint64_t transfer_batches_ = 0;
  uint64_t transfer_word_rects_ = 0;
  uint64_t transfer_words_ = 0;
  uint64_t transfer_depth_direct_ = 0;
  uint64_t transfer_depth_passes_ = 0;
  uint64_t transfer_stencil_passes_ = 0;
  uint64_t transfer_depth_verified_ = 0;
  uint64_t transfer_depth_probe_attempts_ = 0;
  // fh1_debug_transfer_off: claims recorded, transfers not performed.
  uint64_t transfer_skipped_disabled_ = 0;
  uint64_t transfer_verified_ = 0;
  // Resolves whose source was the surface the draws wrote rather than the one
  // the copy's own key named.
  uint64_t owner_resolves_ = 0;
  uint64_t frames_ = 0;
  uint64_t last_stats_frame_ = UINT64_MAX;
};

}  // namespace rex::graphics::d3d11
