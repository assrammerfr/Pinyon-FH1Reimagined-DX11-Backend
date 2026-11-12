/**
 * @file        graphics/d3d11/host_render_config.h
 * @brief       D3D11 backend: host render target configuration (Phase 4)
 *
 *              Host render target configuration the D3D11 pipelines, textures
 *              and the FH1 native render target layer are built against. The
 *              FH1 native route owns the render targets themselves (EDRAM
 *              surface keys -> native ID3D11 textures); conventional host
 *              render targets (RTV / DSV) are the only output-merger path,
 *              there is no EDRAM buffer and no ROV path.
 *
 *              Mirrors the Direct3D 12 backend's D3D12HostRenderConfig.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/d3d11/d3d11_api.h>

namespace rex::graphics::d3d11 {

class D3D11HostRenderConfig {
 public:
  // The draw resolution scale must already be clamped and validated by the
  // caller (the command processor).
  D3D11HostRenderConfig(uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y);

  // Reads the host configuration cvars and queries the device capabilities.
  void Initialize(ID3D11Device* device);

  uint32_t draw_resolution_scale_x() const { return draw_resolution_scale_x_; }
  uint32_t draw_resolution_scale_y() const { return draw_resolution_scale_y_; }
  bool IsDrawResolutionScaled() const {
    return draw_resolution_scale_x_ > 1 || draw_resolution_scale_y_ > 1;
  }

  bool msaa_2x_supported() const { return msaa_2x_supported_; }

  bool gamma_render_target_as_unorm16() const { return gamma_render_target_as_unorm16_; }

  // Using R16G16[B16A16]_SNORM, which are -1...1, not the needed -32...32.
  // Persistent data doesn't depend on this, so can be overriden by per-game
  // configuration.
  bool IsFixed16TruncatedToMinus1To1() const {
    return !REXCVAR_GET(snorm16_render_target_full_range);
  }

  bool depth_float24_round() const { return depth_float24_round_; }
  bool depth_float24_convert_in_pixel_shader() const {
    return depth_float24_convert_in_pixel_shader_;
  }

  DXGI_FORMAT GetColorResourceDXGIFormat(xenos::ColorRenderTargetFormat format) const;
  DXGI_FORMAT GetColorDrawDXGIFormat(xenos::ColorRenderTargetFormat format) const;
  // Formats whose guest EDRAM words are the host channel bits (16-bit or
  // 32-bit float channels besides 32_FLOAT): read and transferred through
  // UINT views.
  static DXGI_FORMAT GetColorUintDXGIFormat(xenos::ColorRenderTargetFormat format);
  static DXGI_FORMAT GetDepthResourceDXGIFormat(xenos::DepthRenderTargetFormat format);
  static DXGI_FORMAT GetDepthDSVDXGIFormat(xenos::DepthRenderTargetFormat format);
  static DXGI_FORMAT GetDepthSRVDepthDXGIFormat(xenos::DepthRenderTargetFormat format);
  static DXGI_FORMAT GetDepthSRVStencilDXGIFormat(xenos::DepthRenderTargetFormat format);

 private:
  uint32_t draw_resolution_scale_x_;
  uint32_t draw_resolution_scale_y_;

  bool gamma_render_target_as_unorm16_ = false;

  bool depth_float24_round_ = false;
  bool depth_float24_convert_in_pixel_shader_ = false;

  bool msaa_2x_supported_ = false;
};

}  // namespace rex::graphics::d3d11
