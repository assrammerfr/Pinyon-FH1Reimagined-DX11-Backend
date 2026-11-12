/**
 * @file        graphics/d3d11/fh1_native_surfaces.h
 * @brief       D3D11 backend: native EDRAM surfaces (Phase 4)
 *
 *              Maps the API-agnostic EDRAM surface keys (fh1_edram_surfaces.h)
 *              to native ID3D11 render targets for the FH1 native route,
 *              including 4x MSAA where the guest requests it (2x emulated with
 *              the top-left and bottom-right samples of 4x MSAA when the host
 *              has no 2x, like the Direct3D 12 executor). The FH1 route owns
 *              its render targets: there is no generic render target cache on
 *              it (mirrors the D3D12 executor's surface layer).
 *
 *              The geometry helpers are device-free so the surface identity
 *              and geometry derivation is unit-testable.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <rex/graphics/d3d11/host_render_config.h>
#include <rex/graphics/fh1_edram_surfaces.h>
#include <rex/ui/d3d11/d3d11_api.h>

namespace rex::graphics::d3d11 {

// A native render target for one EDRAM surface identity, with the views the
// draws (RTV / DSV) and the resolve path (SRVs, UINT RTV) need.
struct D3D11Fh1NativeSurface {
  Fh1SurfaceKey key;
  uint32_t width = 0;   // Guest pixels (unscaled).
  uint32_t height = 0;  // Guest pixels (unscaled).
  uint32_t samples = 1;
  bool stencil_nonzero = false;
  DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;        // RTV / DSV format.
  DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN;         // UINT bits where they differ.
  DXGI_FORMAT stencil_srv_format = DXGI_FORMAT_UNKNOWN;  // Depth only.

  ID3D11Texture2D* texture = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;       // Color (view_format).
  ID3D11RenderTargetView* uint_rtv = nullptr;  // Color raw channel bits, if any.
  ID3D11DepthStencilView* dsv = nullptr;       // Depth.
  ID3D11ShaderResourceView* srv = nullptr;
  ID3D11ShaderResourceView* stencil_srv = nullptr;  // Depth only.
};

// The surface width in guest pixels: EDRAM tiles per row at the surface's
// sample mode.
inline uint32_t Fh1D3D11SurfaceWidth(const Fh1SurfaceKey& key) {
  const uint32_t msaa_x_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k4X));
  return key.pitch_tiles * (xenos::kEdramTileWidthSamples >> msaa_x_log2);
}

// The surface height in guest pixels (fh1_edram_surfaces.h clamps it to the
// guest texture size limit and to the host texture size at the scale).
inline uint32_t Fh1D3D11SurfaceHeight(const Fh1SurfaceKey& key, uint32_t scale) {
  return Fh1SurfaceHeight(key.pitch_tiles, key.msaa, D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION, scale);
}

// Host sample count for a guest sample mode - 2x via 4x (top-left and
// bottom-right samples) where the host has no 2x MSAA.
inline uint32_t Fh1D3D11SurfaceSamples(uint32_t msaa, bool msaa_2x_supported) {
  return msaa == uint32_t(xenos::MsaaSamples::k2X) && !msaa_2x_supported ? 4 : 1u << msaa;
}

// EDRAM surface keys -> native render targets, cleared to zero on creation
// (what the stencil tracking assumes for a new surface).
class D3D11Fh1NativeSurfaces {
 public:
  D3D11Fh1NativeSurfaces(ID3D11Device* device, const D3D11HostRenderConfig& config);
  ~D3D11Fh1NativeSurfaces();

  void Shutdown();

  D3D11Fh1NativeSurface* FindSurface(uint32_t packed_key);
  // Creates the surface (texture + views, cleared to zero) if it doesn't
  // exist yet; nullptr if the identity cannot be represented natively.
  D3D11Fh1NativeSurface* GetOrCreateSurface(const Fh1SurfaceKey& key);

  size_t surface_count() const { return surfaces_.size(); }
  uint32_t created_count() const { return created_count_; }

  // Every identity in the registry, for the one-shot diagnostic dump
  // (fh1_debug_surface_dump): which surfaces hold the frame, by key.
  const std::unordered_map<uint32_t, std::unique_ptr<D3D11Fh1NativeSurface>>& surfaces() const {
    return surfaces_;
  }

 private:
  ID3D11Device* device_;
  const D3D11HostRenderConfig& config_;

  std::unordered_map<uint32_t, std::unique_ptr<D3D11Fh1NativeSurface>> surfaces_;
  uint32_t created_count_ = 0;
};

}  // namespace rex::graphics::d3d11
