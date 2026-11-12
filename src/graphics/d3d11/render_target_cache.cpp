/**
 * @file        graphics/d3d11/render_target_cache.cpp
 * @brief       D3D11 backend skeleton: render target cache (Phase 1)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/render_target_cache.h>

#include <rex/graphics/d3d11/command_processor.h>
#include <rex/logging.h>

namespace rex::graphics::d3d11 {

D3D11RenderTargetCache::~D3D11RenderTargetCache() {
  Shutdown(true);
}

bool D3D11RenderTargetCache::Initialize() {
  InitializeCommon();
  REXGPU_INFO("D3D11: render target cache skeleton initialized (native render targets path)");
  return true;
}

void D3D11RenderTargetCache::Shutdown(bool from_destructor) {
  (void)from_destructor;
}

uint32_t D3D11RenderTargetCache::GetMaxRenderTargetWidth() const {
  // D3D11_REQ_RENDER_TARGET_BUFFER_DIMENSION.
  return 16384;
}

uint32_t D3D11RenderTargetCache::GetMaxRenderTargetHeight() const {
  return 16384;
}

RenderTargetCache::RenderTarget* D3D11RenderTargetCache::CreateRenderTarget(
    RenderTargetKey key) {
  (void)key;
  // The FH1 executor route owns its render targets (Phase 4) - the generic
  // cache has no consumers in this port, and the pixel-shader-interlock
  // fallback path is out of scope on feature level 11_0.
  return nullptr;
}

bool D3D11RenderTargetCache::IsHostDepthEncodingDifferent(
    xenos::DepthRenderTargetFormat format) const {
  // Phase 1 skeleton reports the direct mapping; Phase 4 revisits this for
  // the executor's D24F handling.
  (void)format;
  return false;
}

}  // namespace rex::graphics::d3d11
