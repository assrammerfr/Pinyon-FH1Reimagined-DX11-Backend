/**
 * @file        graphics/d3d11/render_target_cache.h
 * @brief       D3D11 backend skeleton: render target cache (Phase 1)
 *
 *              The FH1 executor route (the only route this port targets)
 *              owns its render targets and does not use the generic EDRAM
 *              render-target cache, and the generic pixel-shader-interlock
 *              fallback path is out of scope for feature level 11_0 (no
 *              rasterizer-ordered views) - so this cache exists to answer
 *              capability queries and reports kHostRenderTargets, matching
 *              the D3D12 backend's model of native render targets.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

#include <rex/graphics/pipeline/render_target/cache.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>
#include <rex/memory.h>

namespace rex::graphics::d3d11 {

class D3D11CommandProcessor;

class D3D11RenderTargetCache final : public RenderTargetCache {
 public:
  D3D11RenderTargetCache(const RegisterFile& register_file, const memory::Memory& memory,
                         uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y,
                         D3D11CommandProcessor& command_processor)
      : RenderTargetCache(register_file, memory, draw_resolution_scale_x, draw_resolution_scale_y),
        command_processor_(command_processor) {}
  ~D3D11RenderTargetCache() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  Path GetPath() const override { return Path::kHostRenderTargets; }
  bool IsGammaFormatHostStorageSeparate() const override { return false; }
  uint32_t GetMaxRenderTargetWidth() const override;
  uint32_t GetMaxRenderTargetHeight() const override;
  RenderTarget* CreateRenderTarget(RenderTargetKey key) override;
  bool IsHostDepthEncodingDifferent(xenos::DepthRenderTargetFormat format) const override;

 private:
  D3D11CommandProcessor& command_processor_;
};

}  // namespace rex::graphics::d3d11
