/**
 * @file        graphics/d3d11/fh1_native_surfaces.cpp
 * @brief       D3D11 backend: native EDRAM surfaces (Phase 4)
 *
 *              Mirrors the Direct3D 12 executor's surface layer: geometry
 *              from the EDRAM surface key, host formats from
 *              D3D11HostRenderConfig, native views for drawing and resolving,
 *              and a zero clear on creation.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/fh1_native_surfaces.h>

#include <rex/logging.h>

namespace rex::graphics::d3d11 {

namespace {

bool CreateView(ID3D11Device* device, ID3D11Texture2D* texture, DXGI_FORMAT format, bool msaa,
                bool depth, bool stencil_plane, ID3D11ShaderResourceView** view) {
  D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.Format = format;
  if (msaa) {
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
  } else {
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipLevels = 1;
    // The plane of planar depth-stencil formats is implied by the view format
    // on Direct3D 11 (unlike Direct3D 12's explicit PlaneSlice).
    (void)stencil_plane;
  }
  return SUCCEEDED(device->CreateShaderResourceView(texture, &desc, view));
}

}  // namespace

D3D11Fh1NativeSurfaces::D3D11Fh1NativeSurfaces(ID3D11Device* device,
                                               const D3D11HostRenderConfig& config)
    : device_(device), config_(config) {}

D3D11Fh1NativeSurfaces::~D3D11Fh1NativeSurfaces() {
  Shutdown();
}

void D3D11Fh1NativeSurfaces::Shutdown() {
  for (auto& entry : surfaces_) {
    D3D11Fh1NativeSurface& surface = *entry.second;
    if (surface.stencil_srv) surface.stencil_srv->Release();
    if (surface.srv) surface.srv->Release();
    if (surface.dsv) surface.dsv->Release();
    if (surface.uint_rtv) surface.uint_rtv->Release();
    if (surface.rtv) surface.rtv->Release();
    if (surface.texture) surface.texture->Release();
  }
  surfaces_.clear();
}

D3D11Fh1NativeSurface* D3D11Fh1NativeSurfaces::FindSurface(uint32_t packed_key) {
  auto it = surfaces_.find(packed_key);
  return it != surfaces_.end() ? it->second.get() : nullptr;
}

D3D11Fh1NativeSurface* D3D11Fh1NativeSurfaces::GetOrCreateSurface(const Fh1SurfaceKey& key) {
  const uint32_t packed = key.Pack();
  if (D3D11Fh1NativeSurface* existing = FindSurface(packed)) return existing;

  auto surface = std::make_unique<D3D11Fh1NativeSurface>();
  surface->key = key;
  surface->width = Fh1D3D11SurfaceWidth(key);
  surface->height = Fh1D3D11SurfaceHeight(key, config_.draw_resolution_scale_x());
  surface->samples = Fh1D3D11SurfaceSamples(key.msaa, config_.msaa_2x_supported());

  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = surface->width * config_.draw_resolution_scale_x();
  desc.Height = surface->height * config_.draw_resolution_scale_x();
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.SampleDesc.Count = surface->samples;
  desc.Usage = D3D11_USAGE_DEFAULT;
  if (key.is_depth) {
    const auto format = xenos::DepthRenderTargetFormat(key.format);
    desc.Format = D3D11HostRenderConfig::GetDepthResourceDXGIFormat(format);
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
    surface->view_format = D3D11HostRenderConfig::GetDepthDSVDXGIFormat(format);
    surface->srv_format = D3D11HostRenderConfig::GetDepthSRVDepthDXGIFormat(format);
    surface->stencil_srv_format = D3D11HostRenderConfig::GetDepthSRVStencilDXGIFormat(format);
  } else {
    const auto format = xenos::ColorRenderTargetFormat(key.format);
    desc.Format = config_.GetColorResourceDXGIFormat(format);
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    surface->view_format = config_.GetColorDrawDXGIFormat(format);
    const DXGI_FORMAT uint_format = D3D11HostRenderConfig::GetColorUintDXGIFormat(format);
    surface->srv_format =
        uint_format != DXGI_FORMAT_UNKNOWN ? uint_format : surface->view_format;
    surface->stencil_srv_format = DXGI_FORMAT_UNKNOWN;
  }
  if (desc.Format == DXGI_FORMAT_UNKNOWN || !surface->width || !surface->height) {
    REXGPU_WARN("D3D11 FH1 native surfaces: cannot represent {} natively",
                key.Describe());
    return nullptr;
  }
  const HRESULT texture_hr = device_->CreateTexture2D(&desc, nullptr, &surface->texture);
  if (FAILED(texture_hr)) {
    REXGPU_ERROR(
        "D3D11 FH1 native surfaces: texture creation failed for {} ({}x{}x{}) hr={:08X}",
        key.Describe(), desc.Width, desc.Height, surface->samples,
        static_cast<uint32_t>(texture_hr));
    return nullptr;
  }

  const bool msaa = surface->samples > 1;
  if (key.is_depth) {
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format = surface->view_format;
    dsv.ViewDimension = msaa ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(device_->CreateDepthStencilView(surface->texture, &dsv, &surface->dsv))) {
      REXGPU_ERROR("D3D11 FH1 native surfaces: DSV creation failed for {}", key.Describe());
      return nullptr;
    }
    if (!CreateView(device_, surface->texture, surface->srv_format, msaa, true, false,
                    &surface->srv) ||
        !CreateView(device_, surface->texture, surface->stencil_srv_format, msaa, true, true,
                    &surface->stencil_srv)) {
      REXGPU_ERROR("D3D11 FH1 native surfaces: depth SRV creation failed for {}",
                   key.Describe());
      return nullptr;
    }
  } else {
    D3D11_RENDER_TARGET_VIEW_DESC rtv = {};
    rtv.Format = surface->view_format;
    rtv.ViewDimension = msaa ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(device_->CreateRenderTargetView(surface->texture, &rtv, &surface->rtv))) {
      REXGPU_ERROR("D3D11 FH1 native surfaces: RTV creation failed for {}", key.Describe());
      return nullptr;
    }
    if (surface->srv_format != surface->view_format) {
      // Transfers write raw channel bits.
      rtv.Format = surface->srv_format;
      if (FAILED(device_->CreateRenderTargetView(surface->texture, &rtv, &surface->uint_rtv))) {
        REXGPU_ERROR("D3D11 FH1 native surfaces: UINT RTV creation failed for {}",
                     key.Describe());
        return nullptr;
      }
    }
    if (!CreateView(device_, surface->texture, surface->srv_format, msaa, false, false,
                    &surface->srv)) {
      REXGPU_ERROR("D3D11 FH1 native surfaces: color SRV creation failed for {}",
                   key.Describe());
      return nullptr;
    }
  }

  // Render targets must be initialized before use; zero is also what the
  // stencil tracking assumes for a new surface. Depth clears like the D3D12
  // executor (24_8 to 1.0, 24_FLOAT to 0.0).
  ID3D11DeviceContext* immediate_context = nullptr;
  device_->GetImmediateContext(&immediate_context);
  if (immediate_context) {
    if (key.is_depth) {
      const float depth_clear =
          xenos::DepthRenderTargetFormat(key.format) == xenos::DepthRenderTargetFormat::kD24S8
              ? 1.0f
              : 0.0f;
      immediate_context->ClearDepthStencilView(
          surface->dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, depth_clear, 0);
    } else {
      static constexpr float kZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      immediate_context->ClearRenderTargetView(surface->rtv, kZero);
    }
    immediate_context->Flush();
    immediate_context->Release();
  }
  ++created_count_;
  REXGPU_INFO("D3D11 FH1 native surfaces: created {} ({}x{}x{} samples)", key.Describe(),
              desc.Width, desc.Height, surface->samples);
  return surfaces_.emplace(packed, std::move(surface)).first->second.get();
}

}  // namespace rex::graphics::d3d11
