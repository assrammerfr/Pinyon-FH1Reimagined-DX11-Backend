/**
 * @file        graphics/d3d11/fh1_native_executor.cpp
 * @brief       D3D11 backend: FH1 native executor (targets, ownership, transfers)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tom@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/fh1_native_executor.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>

#include <rex/cvar.h>
#include <rex/graphics/d3d11/command_processor.h>
#include <rex/graphics/d3d11/host_render_config.h>
#include <rex/graphics/fh1_edram_resolve.h>
#include <rex/graphics/flags.h>
#include <rex/logging.h>

// Defined in graphics/flags.cpp; declared here only, so the owner census does
// not force every translation unit that includes flags.h to rebuild.
REXCVAR_DECLARE(int32_t, fh1_debug_resolve_owner_census);

// The ownership transfer pixel shaders and the fullscreen triangle they draw,
// compiled for this backend's SM 5.0 profile from the same sources the Direct3D
// 12 executor uses (compile-fh1-native.sh's third pass).
namespace shaders {
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_dms_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_dms_from_color_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_dms_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_dms_from_depth_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_dms_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_dms_from_uint_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_from_color_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_from_depth_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_color_from_uint_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_dms_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_dms_from_color_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_dms_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_dms_from_depth_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_dms_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_dms_from_uint_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_from_color_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_from_depth_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_uint_from_uint_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_depth_probe_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_depth_probe_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_dms_from_words_typed_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_from_words_typed_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_dms_from_words_typed_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_from_words_typed_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_words_color_ms_typed_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_words_color_typed_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_words_depth_ms_typed_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_words_depth_typed_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_words_uint_ms_typed_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_words_uint_typed_cs.h"
// The batched words pass: one compute dispatch for every rectangle of one
// destination/source pair (fh1_native_transfer_words_batch.cs.hlsl, built for
// this backend only - the Direct3D 12 executor dispatches per rectangle).
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_words_batch_color_ms_typed_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_words_batch_color_typed_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_words_batch_depth_ms_typed_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_words_batch_depth_typed_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_words_batch_uint_ms_typed_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_words_batch_uint_typed_cs.h"
// The dispatch-free form: the depth and stencil passes straight from the source
// (fh1_native_transfer.ps.hlsl FH1_DEST_KIND 1 and 2, the same shaders the
// Direct3D 12 and Vulkan backends run).
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_dms_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_dms_from_color_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_dms_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_dms_from_depth_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_dms_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_dms_from_uint_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_from_color_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_from_depth_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_depth_from_uint_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_dms_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_dms_from_color_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_dms_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_dms_from_depth_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_dms_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_dms_from_uint_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_from_color_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_from_color_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_from_depth_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_from_depth_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_from_uint_ms_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_transfer_stencil_from_uint_ps.h"
#include "../shaders/bytecode/d3d11_5_0/fullscreen_cw_vs.h"
}  // namespace shaders

namespace rex::graphics::d3d11 {

namespace {

#define FH1_TRANSFER_SHADER(name) \
  { shaders::name, sizeof(shaders::name) }

// Bytes per EDRAM tile at 32bpp (80 x 16 samples of 4 bytes).
constexpr uint32_t kEdramTileBytes32bpp =
    xenos::kEdramTileWidthSamples * xenos::kEdramTileHeightSamples * 4;

// Transfer shader variants: the source's data, and what the destination writes.
// These are the FH1_SOURCE_* / FH1_DEST_KIND defines of fh1_native_transfer.ps.hlsl.
constexpr uint32_t kTransferSourceColor = 0;
constexpr uint32_t kTransferSourceDepth = 1;
constexpr uint32_t kTransferSourceUint = 2;
constexpr uint32_t kTransferDestColor = 0;
// Depth destinations (the EDRAM words pass): the depth value, then one pass per
// stencil bit.
constexpr uint32_t kTransferDestDepth = 1;
constexpr uint32_t kTransferDestStencil = 2;
// Color written as raw channel bits through a UINT view (formats with 16-bit or
// 32-bit float channels, where the transfer must not go through a float target).
constexpr uint32_t kTransferDestUint = 3;

// Rectangles the batched words pass carries in one dispatch
// (fh1_native_transfer_words_batch.cs.hlsl's FH1_MAX_BATCH_RECTS).
constexpr uint32_t kMaxBatchRects = 32;
// A batch whose bounding box is this many times the area of its rectangles
// dispatches per rectangle instead: the threads outside every rectangle would
// cost more than the dispatch saved.
constexpr uint32_t kMaxBatchUnionWaste = 4;

// A pixel rectangle in the transfer shaders' constants: x | y << 16 and
// width | height << 16.
uint32_t PackRectOrigin(const RECT& rect) {
  return uint32_t(rect.left) | (uint32_t(rect.top) << 16);
}
uint32_t PackRectSize(const RECT& rect) {
  return uint32_t(rect.right - rect.left) | (uint32_t(rect.bottom - rect.top) << 16);
}

// Flags of fh1_native_transfer.ps.hlsl (fh1_native_edram.hlsli).
constexpr uint32_t kTransferFlagGammaUnorm16 = 2;
constexpr uint32_t kTransferFlagFixed16FullRange = 4;
constexpr uint32_t kTransferFlagScaleShift = 12;

// Packed EDRAM surface layout for the transfer shader (identical bits to the
// D3D12 and Vulkan executors and to the resolve's PackLayout, decoded in
// fh1_native_edram.hlsli).
uint32_t PackLayout(uint32_t base, uint32_t pitch, uint32_t msaa, bool is_64bpp, bool is_depth,
                    uint32_t format) {
  return (base & 0x7FF) | ((pitch & 0xFF) << 11) | ((msaa & 3) << 19) |
         (uint32_t(is_64bpp) << 21) | (uint32_t(is_depth) << 22) | ((format & 0xF) << 23);
}

// The host's sample mode for a guest 2x surface emulated with 4x MSAA.
uint32_t HostSampleMode(const D3D11Fh1NativeSurface& surface) {
  return surface.key.msaa == uint32_t(xenos::MsaaSamples::k2X)
             ? (surface.samples == 4 ? 2u : 1u)
             : 0u;
}

// What the source surface's data is, as the transfer shader's FH1_SOURCE_*
// defines see it - the same split the resolve uses, because the surface views
// are created the same way (a UINT SRV only for formats with a UINT view).
uint32_t TransferSourceKind(const D3D11Fh1NativeSurface& surface) {
  if (surface.key.is_depth) {
    return kTransferSourceDepth;
  }
  return D3D11HostRenderConfig::GetColorUintDXGIFormat(
             xenos::ColorRenderTargetFormat(surface.key.format)) != DXGI_FORMAT_UNKNOWN
             ? kTransferSourceUint
             : kTransferSourceColor;
}

// Tile geometry of a surface: columns x rows of tiles and the pixel size of one
// tile, derived from the surface itself (the texture that was created) so the
// mapping from EDRAM tile indices to pixels is the surface's own.
struct TileGeometry {
  uint32_t columns = 0;
  uint32_t rows = 0;
  uint32_t pixel_width = 0;
  uint32_t pixel_height = 0;
  uint32_t surface_width = 0;
  uint32_t surface_height = 0;
};

bool GetTileGeometry(const D3D11Fh1NativeSurface& surface, TileGeometry& geometry) {
  const uint32_t msaa_y_log2 = uint32_t(surface.key.msaa >= uint32_t(xenos::MsaaSamples::k2X));
  if (!surface.key.pitch_tiles || !surface.width || !surface.height) {
    return false;
  }
  geometry.columns = surface.key.pitch_tiles;
  geometry.pixel_width = surface.width / geometry.columns;
  geometry.pixel_height = xenos::kEdramTileHeightSamples >> msaa_y_log2;
  if (!geometry.pixel_width || !geometry.pixel_height) {
    return false;
  }
  geometry.rows = surface.height / geometry.pixel_height;
  geometry.surface_width = surface.width;
  geometry.surface_height = surface.height;
  return geometry.rows != 0;
}

// The pixels of a run of tiles that is contiguous in EDRAM tile indices: the
// rest of the first tile row, whole rows, and the start of the last row - at
// most three rectangles (the Direct3D 12 executor's decomposition).
uint32_t RunRects(const TileGeometry& geometry, uint32_t first, uint32_t count, RECT* rects) {
  const uint32_t pitch = geometry.columns;
  const uint32_t tiles = pitch * geometry.rows;
  if (!pitch || !tiles || first >= tiles || !count) {
    return 0;
  }
  const uint32_t end = std::min(first + count, tiles);
  uint32_t rect_count = 0;
  auto add_rect = [&](uint32_t column_first, uint32_t row_first, uint32_t column_end,
                      uint32_t row_end) {
    if (rect_count >= 3 || column_first >= column_end || row_first >= row_end) {
      return;
    }
    RECT rect = {LONG(column_first * geometry.pixel_width), LONG(row_first * geometry.pixel_height),
                 LONG(std::min(column_end * geometry.pixel_width, geometry.surface_width)),
                 LONG(std::min(row_end * geometry.pixel_height, geometry.surface_height))};
    if (rect.left < rect.right && rect.top < rect.bottom) {
      rects[rect_count++] = rect;
    }
  };
  const uint32_t first_row = first / pitch, last_row = (end - 1) / pitch;
  if (first_row == last_row) {
    add_rect(first % pitch, first_row, (end - 1) % pitch + 1, first_row + 1);
  } else {
    uint32_t full_first = first_row;
    if (first % pitch) {
      add_rect(first % pitch, first_row, pitch, first_row + 1);
      ++full_first;
    }
    uint32_t full_end = last_row + 1;
    if (end % pitch) {
      --full_end;
      add_rect(0, last_row, end % pitch, last_row + 1);
    }
    if (full_first < full_end) {
      add_rect(0, full_first, pitch, full_end);
    }
  }
  return rect_count;
}

// Bytes per pixel of the formats the EDRAM surfaces are created with.
uint32_t BytesPerPixel(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
      return 8;
    default:
      return 4;
  }
}

// The pipeline variant key of a transfer shader: source kind and MSAA, then the
// destination's.
uint32_t TransferShaderKey(uint32_t source_kind, bool source_msaa, uint32_t dest_kind,
                           bool dest_msaa) {
  return (source_kind & 3) | (uint32_t(source_msaa) << 2) | (uint32_t(dest_kind & 3) << 3) |
         (uint32_t(dest_msaa) << 5);
}

}  // namespace

D3D11Fh1NativeExecutor::D3D11Fh1NativeExecutor(D3D11CommandProcessor& command_processor,
                                               const RegisterFile& register_file)
    : command_processor_(command_processor), register_file_(register_file) {}

D3D11Fh1NativeExecutor::~D3D11Fh1NativeExecutor() { Shutdown(); }

bool D3D11Fh1NativeExecutor::Initialize(const D3D11Fh1NativeExecutorConfig& config) {
  if (initialized_) {
    return true;
  }
  if (!config.surfaces) {
    REXGPU_ERROR("D3D11 FH1 native executor: no native surface registry");
    return false;
  }
  if (config.scale != 1) {
    // The executor executes scale 1 only (the surfaces, the resolve and the
    // guest layout all agree at 1x). Refused rather than drawn into wrongly
    // sized targets.
    REXGPU_ERROR(
        "D3D11 FH1 native executor: draw resolution scale {} is not supported (scale 1 only)",
        config.scale);
    return false;
  }
  config_ = config;
  tiles_.Reset();
  initialized_ = true;
  REXGPU_INFO(
      "D3D11 FH1 native executor: initialized (scale 1, EDRAM tiles {} x {} B, shared with the "
      "surface registry the resolve reads)",
      xenos::kEdramTileCount, kEdramTileBytes32bpp);
  return true;
}

void D3D11Fh1NativeExecutor::Shutdown() {
  if (!initialized_) {
    return;
  }
  LogStats(frames_);
  initialized_ = false;
  pending_targets_valid_ = false;
  pending_used_bits_ = 0;
  for (D3D11Fh1NativeSurface*& surface : pending_surfaces_) {
    surface = nullptr;
  }
  for (auto& entry : transfer_pixel_shaders_) {
    if (entry.second) {
      entry.second->Release();
    }
  }
  transfer_pixel_shaders_.clear();
  for (auto& entry : transfer_words_shaders_) {
    if (entry.second) {
      entry.second->Release();
    }
  }
  transfer_words_shaders_.clear();
  for (auto& entry : transfer_words_batch_shaders_) {
    if (entry.second) {
      entry.second->Release();
    }
  }
  transfer_words_batch_shaders_.clear();
  for (auto& entry : transfer_from_words_shaders_) {
    if (entry.second) {
      entry.second->Release();
    }
  }
  transfer_from_words_shaders_.clear();
  for (auto& entry : transfer_direct_shaders_) {
    if (entry.second) {
      entry.second->Release();
    }
  }
  transfer_direct_shaders_.clear();
  queued_rects_.clear();
  queued_chunks_.clear();
  for (auto& entry : transfer_depth_states_) {
    if (entry.second) {
      entry.second->Release();
    }
  }
  transfer_depth_states_.clear();
  if (transfer_words_srv_) {
    transfer_words_srv_->Release();
    transfer_words_srv_ = nullptr;
  }
  if (transfer_words_uav_) {
    transfer_words_uav_->Release();
    transfer_words_uav_ = nullptr;
  }
  if (transfer_words_buffer_) {
    transfer_words_buffer_->Release();
    transfer_words_buffer_ = nullptr;
  }
  transfer_words_capacity_ = 0;
  if (depth_probe_shader_) {
    depth_probe_shader_->Release();
    depth_probe_shader_ = nullptr;
  }
  if (depth_probe_shader_ms_) {
    depth_probe_shader_ms_->Release();
    depth_probe_shader_ms_ = nullptr;
  }
  if (transfer_vertex_shader_) {
    transfer_vertex_shader_->Release();
    transfer_vertex_shader_ = nullptr;
  }
  if (transfer_constants_) {
    transfer_constants_->Release();
    transfer_constants_ = nullptr;
  }
  if (transfer_rasterizer_state_) {
    transfer_rasterizer_state_->Release();
    transfer_rasterizer_state_ = nullptr;
  }
  if (transfer_depth_stencil_state_) {
    transfer_depth_stencil_state_->Release();
    transfer_depth_stencil_state_ = nullptr;
  }
  tiles_.Reset();
}

D3D11Fh1NativeExecutor::SurfaceKey D3D11Fh1NativeExecutor::MakeColorKey(
    uint32_t base, uint32_t pitch_tiles, uint32_t msaa,
    xenos::ColorRenderTargetFormat format) const {
  return SurfaceKey::Color(base, pitch_tiles, msaa, format, config_.gamma_as_unorm16);
}

D3D11Fh1NativeExecutor::SurfaceKey D3D11Fh1NativeExecutor::MakeDepthKey(
    uint32_t base, uint32_t pitch_tiles, uint32_t msaa, xenos::DepthRenderTargetFormat format) {
  return SurfaceKey::Depth(base, pitch_tiles, msaa, format);
}

uint32_t D3D11Fh1NativeExecutor::PitchTiles(uint32_t pitch_pixels, uint32_t msaa) {
  // The tree's one derivation (fh1_edram_surfaces.h), which the render target
  // path and the resolve both use: a different rounding here would derive a
  // different surface key and bind surfaces the resolve never reads.
  return Fh1PitchTiles(pitch_pixels, msaa);
}

uint32_t D3D11Fh1NativeExecutor::SurfaceHeight(uint32_t pitch_tiles, uint32_t msaa) const {
  return Fh1SurfaceHeight(pitch_tiles, msaa, D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION, config_.scale);
}

void D3D11Fh1NativeExecutor::PrepareTargets(const Fh1DrawInfo& draw, int32_t claim_x0,
                                            int32_t claim_y0, int32_t claim_x1, int32_t claim_y1) {
  if (!initialized_) {
    return;
  }
  ++prepares_;
  pending_targets_valid_ = true;
  pending_used_bits_ = 0;
  for (D3D11Fh1NativeSurface*& surface : pending_surfaces_) {
    surface = nullptr;
  }
  if (draw.memexport) {
    return;
  }
  const RegisterFile& regs = register_file_;

  // Targets the draw writes, derived the way the guest GPU addresses EDRAM:
  // depth when tested or stencilled, colors with a nonzero write mask, the
  // lower slot winning a shared base.
  const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  const uint32_t msaa = uint32_t(surface_info.msaa_samples);
  const uint32_t pitch_tiles = PitchTiles(surface_info.surface_pitch, msaa);
  uint32_t used_bits = 0;
  if (draw.rasterization_done && pitch_tiles) {
    if (draw.normalized_depth_control.z_enable ||
        draw.normalized_depth_control.stencil_enable) {
      const auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
      pending_keys_[0] =
          MakeDepthKey(depth_info.depth_base, pitch_tiles, msaa, depth_info.depth_format);
      used_bits |= 1;
    }
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (!(draw.normalized_color_mask & (0xFu << (4 * i)))) {
        continue;
      }
      const auto color_info =
          regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
      pending_keys_[1 + i] =
          MakeColorKey(color_info.color_base, pitch_tiles, msaa, color_info.color_format);
      used_bits |= 1u << (1 + i);
    }
    for (uint32_t i = 1; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      if (!(used_bits & (1u << i))) {
        continue;
      }
      for (uint32_t j = 1; j < 1 + xenos::kMaxColorRenderTargets; ++j) {
        if (j != i && j > i && (used_bits & (1u << j)) &&
            pending_keys_[j].base_tiles == pending_keys_[i].base_tiles) {
          used_bits &= ~(1u << j);
        }
      }
    }
  }
  if (!used_bits) {
    pending_used_bits_ = 0;
    return;
  }

  // The surfaces exist before the claims: a transfer needs the destination's
  // texture, and the claim is what decides whether there is anything to
  // transfer. BindTargets binds what is kept here.
  const bool depth_stencil_written =
      (used_bits & 1) &&
      (draw.normalized_depth_control.stencil_enable || draw.normalized_depth_control.z_enable);
  for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (!(used_bits & (1u << i))) {
      continue;
    }
    D3D11Fh1NativeSurface* surface = config_.surfaces->GetOrCreateSurface(pending_keys_[i]);
    if (!surface) {
      // Nothing to bind for this slot; reported as unbound to the pipeline key.
      used_bits &= ~(1u << i);
      continue;
    }
    pending_surfaces_[i] = surface;
    // The low byte of a color tile's words is color, not stencil.
    ClaimTargetTiles(pending_keys_[i], *surface, claim_x0, claim_y0, claim_x1, claim_y1,
                     (i == 0) && depth_stencil_written);
  }
  pending_used_bits_ = used_bits;
}

void D3D11Fh1NativeExecutor::ClaimTargetTiles(const SurfaceKey& key,
                                              D3D11Fh1NativeSurface& surface, int32_t x0,
                                              int32_t y0, int32_t x1, int32_t y1, bool stencil) {
  TileGeometry geometry;
  if (!GetTileGeometry(surface, geometry)) {
    return;
  }
  // The draw's rectangle, clamped to the surface. A zero-sized one claims
  // nothing: the draw cannot write a pixel.
  const int32_t left = std::max(x0, 0), top = std::max(y0, 0);
  const int32_t right = std::min(x1, int32_t(geometry.surface_width));
  const int32_t bottom = std::min(y1, int32_t(geometry.surface_height));
  if (left >= right || top >= bottom) {
    return;
  }
  const uint32_t column_first = uint32_t(left) / geometry.pixel_width;
  const uint32_t column_end = std::min(
      (uint32_t(right) + geometry.pixel_width - 1) / geometry.pixel_width, geometry.columns);
  const uint32_t row_first = uint32_t(top) / geometry.pixel_height;
  const uint32_t row_end = std::min(
      (uint32_t(bottom) + geometry.pixel_height - 1) / geometry.pixel_height, geometry.rows);
  if (column_first >= column_end || row_first >= row_end) {
    return;
  }

  const uint32_t packed_key = key.Pack();
  const uint32_t pitch = key.pitch_tiles;
  const uint32_t row_tiles = column_end - column_first;
  ++claims_;
  claimed_tiles_ += uint64_t(row_tiles) * (row_end - row_first);

  // One ClaimRect per draw rather than one Claim per tile: it resolves the
  // whole rectangle's previous owner in a single pass, and per-row claims only
  // when the rectangle mixes owners (or already holds the claimant's tiles).
  const Fh1EdramTiles::RectClaim claim = tiles_.ClaimRect(
      key.base_tiles, pitch, packed_key, column_first, row_first, column_end, row_end);
  if (claim.per_row) {
    for (uint32_t row = row_first; row < row_end; ++row) {
      const uint32_t row_first_tile = key.base_tiles + row * pitch + column_first;
      const std::vector<Fh1EdramTiles::Run> runs =
          tiles_.Claim(row_first_tile, row_tiles, packed_key, true);
      for (const Fh1EdramTiles::Run& run : runs) {
        QueueTransferRun(key, surface, run.previous_owner, run.first, run.count);
      }
      if (stencil) {
        tiles_.MarkStencil(row_first_tile, row_tiles, true);
      }
    }
    FlushClaimTransfers(key, surface);
    return;
  }
  const uint32_t previous_owner = claim.previous_owner;
  if (previous_owner != Fh1EdramTiles::kNoOwner) {
    if (column_first == 0 && column_end == pitch) {
      // Rows are contiguous in EDRAM tiles when the rectangle spans the pitch.
      QueueTransferRun(key, surface, previous_owner, key.base_tiles + row_first * pitch,
                       (row_end - row_first) * pitch);
    } else {
      for (uint32_t row = row_first; row < row_end; ++row) {
        QueueTransferRun(key, surface, previous_owner, key.base_tiles + row * pitch + column_first,
                         row_tiles);
      }
    }
  }
  if (stencil) {
    for (uint32_t row = row_first; row < row_end; ++row) {
      tiles_.MarkStencil(key.base_tiles + row * pitch + column_first, row_tiles, true);
    }
  }
  FlushClaimTransfers(key, surface);
}

void D3D11Fh1NativeExecutor::QueueTransferRun(const SurfaceKey& dest_key,
                                             D3D11Fh1NativeSurface& dest,
                                             uint32_t previous_owner, uint32_t first_tile,
                                             uint32_t tile_count) {
  if (!tile_count || previous_owner == Fh1EdramTiles::kNoOwner ||
      previous_owner == dest_key.Pack()) {
    return;
  }
  if (REXCVAR_GET(fh1_debug_transfer_off)) {
    // Bisect: the claims and the previous owner are recorded and counted, the
    // content is not moved.
    ++transfer_skipped_disabled_;
    return;
  }
  TileGeometry geometry;
  if (!GetTileGeometry(dest, geometry)) {
    return;
  }
  // Tile indices wrap around EDRAM (2048 tiles), which is also a wrap from the
  // surface's last row to its first: a run that crosses it is two runs.
  const uint32_t mask = xenos::kEdramTileCount - 1;
  uint32_t first = (first_tile - dest_key.base_tiles) & mask;
  uint32_t remaining = std::min(tile_count, uint32_t(xenos::kEdramTileCount));
  RECT rects[3];
  while (remaining) {
    const uint32_t chunk = std::min(remaining, uint32_t(xenos::kEdramTileCount) - first);
    const uint32_t rect_count = RunRects(geometry, first, chunk, rects);
    if (rect_count) {
      // The stencil state is read now, not at the flush: the claim's own
      // MarkStencil calls come after the runs it queued.
      QueuedChunk queued;
      queued.previous_owner = previous_owner;
      queued.stencil = tiles_.AnyStencil(first, chunk);
      queued.tile_count = chunk;
      queued.rect_first = uint32_t(queued_rects_.size());
      queued.rect_count = rect_count;
      queued_rects_.insert(queued_rects_.end(), rects, rects + rect_count);
      queued_chunks_.push_back(queued);
    }
    remaining -= chunk;
    first = 0;
  }
}

void D3D11Fh1NativeExecutor::FlushClaimTransfers(const SurfaceKey& dest_key,
                                                D3D11Fh1NativeSurface& dest) {
  if (queued_chunks_.empty()) {
    return;
  }
  // Bisect: every run transfers on its own, the way the depth tail landed.
  if (REXCVAR_GET(fh1_debug_transfer_unbatched)) {
    for (const QueuedChunk& chunk : queued_chunks_) {
      TransferRects(dest_key, dest, chunk.previous_owner, &queued_rects_[chunk.rect_first],
                    chunk.rect_count, chunk.tile_count, chunk.stencil);
    }
    queued_rects_.clear();
    queued_chunks_.clear();
    return;
  }
  // One transfer per (previous owner, stencil) pair: the pair's rectangles are
  // disjoint, so they can share a dispatch and a set of passes. Their order
  // inside the pair is the order they were claimed in.
  std::vector<bool> issued(queued_chunks_.size(), false);
  RECT batch[kMaxBatchRects];
  for (size_t first = 0; first < queued_chunks_.size(); ++first) {
    if (issued[first]) {
      continue;
    }
    const uint32_t previous_owner = queued_chunks_[first].previous_owner;
    const bool stencil = queued_chunks_[first].stencil;
    uint32_t rect_count = 0, tile_count = 0;
    for (size_t i = first; i < queued_chunks_.size(); ++i) {
      const QueuedChunk& chunk = queued_chunks_[i];
      if (issued[i] || chunk.previous_owner != previous_owner ||
          chunk.stencil != stencil) {
        continue;
      }
      if (rect_count + chunk.rect_count > kMaxBatchRects) {
        TransferRects(dest_key, dest, previous_owner, batch, rect_count, tile_count, stencil);
        rect_count = 0;
        tile_count = 0;
      }
      for (uint32_t r = 0; r < chunk.rect_count; ++r) {
        batch[rect_count++] = queued_rects_[chunk.rect_first + r];
      }
      tile_count += chunk.tile_count;
      issued[i] = true;
    }
    if (rect_count) {
      TransferRects(dest_key, dest, previous_owner, batch, rect_count, tile_count, stencil);
    }
  }
  queued_rects_.clear();
  queued_chunks_.clear();
}

void D3D11Fh1NativeExecutor::TransferRects(const SurfaceKey& dest_key,
                                           D3D11Fh1NativeSurface& dest,
                                           uint32_t previous_owner, const RECT* rects,
                                           uint32_t rect_count, uint32_t tile_count,
                                           bool stencil) {
  if (!rect_count || !config_.surfaces || !dest.texture) {
    return;
  }
  D3D11Fh1NativeSurface* source = config_.surfaces->FindSurface(previous_owner);
  if (!source || !source->texture) {
    // The previous owner never got a surface in this session (or it was
    // released): nothing to inherit, the claimant's own cleared content stays.
    ++transfer_skipped_source_;
    return;
  }
  ID3D11DeviceContext* context = command_processor_.GetD3D11Provider().GetImmediateContext();
  if (!context) {
    return;
  }
  D3D11_TEXTURE2D_DESC dest_desc = {}, source_desc = {};
  dest.texture->GetDesc(&dest_desc);
  source->texture->GetDesc(&source_desc);

  // The same host format and geometry is a plain copy of the tiles: exact, and
  // cheaper than the shader.
  const bool same_geometry = dest_key.pitch_tiles == source->key.pitch_tiles &&
                             dest_key.msaa == source->key.msaa &&
                             dest_key.is_depth == source->key.is_depth;
  if (same_geometry && dest_desc.Format == source_desc.Format &&
      dest_desc.SampleDesc.Count == source_desc.SampleDesc.Count) {
    // A depth copy is the clearest probe of all: the same rectangle of the same
    // format in both surfaces, so the claimant's depth must end up holding its
    // predecessor's texel for texel.
    int64_t probe_before = -1;
    const bool probe = REXCVAR_GET(fh1_swap_fetch_trace) && dest_key.is_depth &&
                       transfer_depth_verified_ < 8 && transfer_depth_probe_attempts_ < 60 &&
                       dest_desc.SampleDesc.Count == 1;
    if (probe) {
      ++transfer_depth_probe_attempts_;
      probe_before = CountEmptyDepth(dest, rects[0]);
    }
    for (uint32_t i = 0; i < rect_count; ++i) {
      const D3D11_BOX box = {UINT(rects[i].left), UINT(rects[i].top), 0, UINT(rects[i].right),
                             UINT(rects[i].bottom), 1};
      context->CopySubresourceRegion(dest.texture, 0, box.left, box.top, 0, source->texture, 0,
                                     &box);
    }
    ++transfers_;
    ++transfer_copies_;
    transfer_tiles_ += tile_count;
    if (probe_before >= 0) {
      VerifyDepthTransfer(dest, *source, rects[0], uint64_t(probe_before));
    }
    return;
  }

  // Everything else goes through the transfer shader, which reads the previous
  // owner's words through its layout and format and writes them through the
  // claimant's. A depth destination cannot be written by one color pass (depth
  // and stencil are different outputs), so it goes through the EDRAM words pass
  // - the Direct3D 12 executor's transfer_words and from_words - where a source
  // of any kind is supported.
  if (dest_key.is_depth) {
    TransferDepthRects(dest_key, dest, *source, rects, rect_count, tile_count, stencil);
    return;
  }
  if (!Fh1IsResolveColorFormatSupported(xenos::ColorRenderTargetFormat(dest_key.format)) ||
      (!source->key.is_depth &&
       !Fh1IsResolveColorFormatSupported(
           xenos::ColorRenderTargetFormat(source->key.format)))) {
    ++transfer_skipped_format_;
    return;
  }
  const uint32_t source_kind = TransferSourceKind(*source);
  // A destination whose texture has a UINT view must be written through it: its
  // channels are 16- or 32-bit floats, which a float render target would clip.
  const uint32_t dest_kind = dest.uint_rtv ? kTransferDestUint : kTransferDestColor;
  ID3D11RenderTargetView* render_target = dest_kind == kTransferDestUint ? dest.uint_rtv : dest.rtv;
  if (!EnsureTransferResources()) {
    ++transfer_skipped_format_;
    return;
  }
  ID3D11PixelShader* pixel_shader =
      GetTransferShader(source_kind, source->samples > 1, dest_kind, dest.samples > 1);
  if (!render_target || !pixel_shader || !source->srv) {
    ++transfer_skipped_format_;
    return;
  }
  if (source_kind == kTransferSourceDepth && !source->stencil_srv) {
    ++transfer_skipped_format_;
    return;
  }

  const uint32_t host_sample_mode_shift = 27;
  const uint32_t flags = (config_.gamma_as_unorm16 ? kTransferFlagGammaUnorm16 : 0u) |
                         (config_.fixed16_full_range ? kTransferFlagFixed16FullRange : 0u) |
                         ((config_.scale - 1) << kTransferFlagScaleShift);
  const uint32_t constants[4] = {
      PackLayout(dest_key.base_tiles, dest_key.pitch_tiles, dest_key.msaa, dest_key.Is64bpp(),
                 dest_key.is_depth, dest_key.format) |
          (HostSampleMode(dest) << host_sample_mode_shift),
      PackLayout(source->key.base_tiles, source->key.pitch_tiles, source->key.msaa,
                 source->key.Is64bpp(), source->key.is_depth, source->key.format) |
          (HostSampleMode(*source) << host_sample_mode_shift),
      flags, 0u};
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(context->Map(transfer_constants_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    ++transfer_skipped_format_;
    return;
  }
  std::memcpy(mapped.pData, constants, sizeof(constants));
  context->Unmap(transfer_constants_, 0);

  context->OMSetRenderTargets(1, &render_target, nullptr);
  context->OMSetDepthStencilState(transfer_depth_stencil_state_, 0);
  context->RSSetState(transfer_rasterizer_state_);
  context->IASetInputLayout(nullptr);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
  context->VSSetShader(transfer_vertex_shader_, nullptr, 0);
  context->PSSetShader(pixel_shader, nullptr, 0);
  context->PSSetConstantBuffers(0, 1, &transfer_constants_);
  ID3D11ShaderResourceView* source_views[2] = {source->srv,
                                               source_kind == kTransferSourceDepth
                                                   ? source->stencil_srv
                                                   : nullptr};
  context->PSSetShaderResources(0, source_kind == kTransferSourceDepth ? 2u : 1u, source_views);
  // fh1_swap_fetch_trace: the first few transfers of a session are read back -
  // the claimant's rectangle before the draw, after it, and the predecessor's
  // for the same rectangle (same geometry and single sample only, so the pixel
  // rectangle means the same thing in both surfaces).
  int64_t probe_before = -1;
  const bool probe = REXCVAR_GET(fh1_swap_fetch_trace) && transfer_verified_ < 8 &&
                     dest_key.pitch_tiles == source->key.pitch_tiles &&
                     dest_key.msaa == source->key.msaa && dest.samples == 1 &&
                     source->samples == 1;
  if (probe) {
    probe_before = CountZeroTexels(dest.texture, rects[0]);
  }
  for (uint32_t i = 0; i < rect_count; ++i) {
    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = float(rects[i].left);
    viewport.TopLeftY = float(rects[i].top);
    viewport.Width = float(rects[i].right - rects[i].left);
    viewport.Height = float(rects[i].bottom - rects[i].top);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);
    context->RSSetScissorRects(1, &rects[i]);
    context->Draw(3, 0);
  }
  ++transfers_;
  ++transfer_draws_;
  transfer_tiles_ += tile_count;
  if (probe_before >= 0) {
    VerifyTransfer(dest, *source, rects[0], uint64_t(probe_before));
  }
}

int64_t D3D11Fh1NativeExecutor::CountZeroTexels(ID3D11Texture2D* texture, const RECT& rect) {
  ID3D11Device* device = command_processor_.GetD3D11Provider().GetDevice();
  ID3D11DeviceContext* context = command_processor_.GetD3D11Provider().GetImmediateContext();
  const LONG width = rect.right - rect.left, height = rect.bottom - rect.top;
  if (!device || !context || !texture || width <= 0 || height <= 0) {
    return -1;
  }
  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);
  if (UINT(width) > desc.Width || UINT(height) > desc.Height) {
    return -1;
  }
  D3D11_TEXTURE2D_DESC staging_desc = desc;
  staging_desc.Width = UINT(width);
  staging_desc.Height = UINT(height);
  staging_desc.SampleDesc.Count = 1;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging_desc.MiscFlags = 0;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, &staging))) {
    return -1;
  }
  const D3D11_BOX box = {UINT(rect.left), UINT(rect.top), 0, UINT(rect.right), UINT(rect.bottom),
                         1};
  context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture, 0, &box);
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return -1;
  }
  // A texel is empty when all of its bytes are zero, whatever the channel
  // widths: a byte count would call a small value "content" and a wide format's
  // padding "content" alike, and the two sides of a transfer can have different
  // widths (8_8_8_8_GAMMA is hosted as 16-bit channels, 2_10_10_10 as a packed
  // word).
  const uint32_t bytes_per_pixel = BytesPerPixel(desc.Format);
  const uint32_t row_bytes =
      std::min<uint32_t>(uint32_t(width) * bytes_per_pixel, mapped.RowPitch);
  int64_t zero_texels = 0;
  for (LONG y = 0; y < height; ++y) {
    const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + size_t(mapped.RowPitch) * y;
    for (LONG x = 0; x < width && uint32_t(x + 1) * bytes_per_pixel <= row_bytes; ++x) {
      uint32_t any = 0;
      for (uint32_t b = 0; b < bytes_per_pixel; ++b) {
        any |= row[size_t(x) * bytes_per_pixel + b];
      }
      zero_texels += any == 0;
    }
  }
  context->Unmap(staging.Get(), 0);
  return zero_texels;
}

void D3D11Fh1NativeExecutor::VerifyTransfer(D3D11Fh1NativeSurface& dest,
                                            D3D11Fh1NativeSurface& source, const RECT& rect,
                                            uint64_t dest_before) {
  const int64_t dest_after = CountZeroTexels(dest.texture, rect);
  const int64_t source_zero = CountZeroTexels(source.texture, rect);
  if (dest_after < 0 || source_zero < 0) {
    return;
  }
  ++transfer_verified_;
  const uint64_t texels =
      uint64_t(rect.right - rect.left) * uint64_t(rect.bottom - rect.top);
  // Empty texels, not bytes, on both sides: a faithful transfer makes the
  // claimant's empty texels those of its predecessor. `before` is the
  // claimant's own content, replaced by the predecessor's words - which is what
  // ownership means, and what makes the difference visible when the previous
  // owner had never drawn the tiles its claim covered.
  const uint64_t tolerance = std::max<uint64_t>(1, texels / 100);
  const bool faithful =
      uint64_t(std::llabs(dest_after - source_zero)) <= tolerance;
  REXGPU_INFO(
      "D3D11 FH1 native executor: transfer verify {} -> {} rect={},{}-{},{} texels={} "
      "empty_texels dest_before={} dest_after={} source={} ({})",
      source.key.Describe(), dest.key.Describe(), rect.left, rect.top, rect.right, rect.bottom,
      texels, dest_before, uint64_t(dest_after), uint64_t(source_zero),
      faithful ? "the claimant now holds its predecessor's words" : "MISMATCH");
}

bool D3D11Fh1NativeExecutor::TransferDirectSupported(
    const D3D11Fh1NativeSurface& source) const {
  // The direct pass is the words path without the words: same LoadSourceWord,
  // same depth decode, same stencil bits, read straight from the previous
  // owner. One difference matters - fh1_native_transfer.ps.hlsl leaves
  // fh1_fixed16_scale at its 32x default for the depth and stencil kinds, while
  // the words pass sets it from the fixed16 flag - and it only reaches a
  // 16_16[_16_16] color source, whose channels DecodeColor scales. Those stay
  // on the words path unless the host keeps the full range (so that both agree
  // on 32).
  if (source.key.is_depth || config_.fixed16_full_range) {
    return true;
  }
  const uint32_t format = source.key.format;
  return format != uint32_t(xenos::ColorRenderTargetFormat::k_16_16) &&
         format != uint32_t(xenos::ColorRenderTargetFormat::k_16_16_16_16);
}

void D3D11Fh1NativeExecutor::TransferDepthRects(const SurfaceKey& dest_key,
                                                D3D11Fh1NativeSurface& dest,
                                                D3D11Fh1NativeSurface& source, const RECT* rects,
                                                uint32_t rect_count, uint32_t tile_count,
                                                bool stencil) {
  if (!rect_count || !dest.texture || !dest.dsv) {
    ++transfer_skipped_format_;
    return;
  }
  ID3D11Device* device = command_processor_.GetD3D11Provider().GetDevice();
  ID3D11DeviceContext* context = command_processor_.GetD3D11Provider().GetImmediateContext();
  if (!device || !context || !EnsureTransferResources()) {
    ++transfer_skipped_format_;
    return;
  }
  const uint32_t source_kind = TransferSourceKind(source);
  if (!source.srv || (source_kind == kTransferSourceDepth && !source.stencil_srv)) {
    ++transfer_skipped_format_;
    return;
  }
  const bool dest_msaa = dest.samples > 1;
  // Direct: the depth and stencil passes read the previous owner's surface
  // themselves - no words buffer, no UAV, no compute dispatch. That is
  // fh1_native_transfer.ps.hlsl's FH1_DEST_KIND 1 and 2, the form the Direct3D
  // 12 and Vulkan backends already run; the words pass stays for a source whose
  // words the direct shader would compute differently (TransferDirectSupported),
  // and is what runs unless fh1_debug_transfer_direct asks for the other.
  const bool direct = TransferDirectSupported(source) && REXCVAR_GET(fh1_debug_transfer_direct);
  ID3D11ComputeShader* words_shader =
      direct ? nullptr : GetTransferWordsBatchShader(source_kind, source.samples > 1);
  ID3D11PixelShader* depth_shader =
      direct ? GetTransferDirectShader(source_kind, source.samples > 1, kTransferDestDepth,
                                       dest_msaa)
             : GetTransferFromWordsShader(kTransferDestDepth, dest_msaa);
  ID3D11PixelShader* stencil_shader =
      !stencil ? nullptr
               : (direct ? GetTransferDirectShader(source_kind, source.samples > 1,
                                                   kTransferDestStencil, dest_msaa)
                         : GetTransferFromWordsShader(kTransferDestStencil, dest_msaa));
  const uint32_t dest_width = dest.width * config_.scale;
  const uint32_t dest_height = dest.height * config_.scale;
  if (!depth_shader || (stencil && !stencil_shader) ||
      (!direct &&
       (!words_shader ||
        !EnsureTransferWords(uint64_t(dest_width) * dest_height * dest.samples)))) {
    ++transfer_skipped_format_;
    return;
  }

  const uint32_t flags = (config_.gamma_as_unorm16 ? kTransferFlagGammaUnorm16 : 0u) |
                         (config_.fixed16_full_range ? kTransferFlagFixed16FullRange : 0u) |
                         ((config_.scale - 1) << kTransferFlagScaleShift);
  constexpr uint32_t kHostSampleModeShift = 27;
  const uint32_t dest_layout =
      PackLayout(dest_key.base_tiles, dest_key.pitch_tiles, dest_key.msaa, dest_key.Is64bpp(),
                 dest_key.is_depth, dest_key.format) |
      (HostSampleMode(dest) << kHostSampleModeShift);
  const uint32_t source_layout =
      PackLayout(source.key.base_tiles, source.key.pitch_tiles, source.key.msaa,
                 source.key.Is64bpp(), source.key.is_depth, source.key.format) |
      (HostSampleMode(source) << kHostSampleModeShift);
  auto set_constants = [&](const uint32_t* data, uint32_t dwords) -> bool {
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(transfer_constants_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      return false;
    }
    std::memcpy(mapped.pData, data, sizeof(uint32_t) * dwords);
    context->Unmap(transfer_constants_, 0);
    return true;
  };

  // The source may still be bound as the target the draw that wrote it left
  // bound: Direct3D 11 drops the shader resource bind of a resource bound for
  // output, silently. Unbind the outputs; every draw binds its own again.
  {
    ID3D11RenderTargetView* no_targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, no_targets, nullptr);
  }

  // First step (words path): the previous owner's guest EDRAM word for every
  // destination sample of each rectangle, once. The words buffer is a typed
  // R32_UINT element per word - no raw view is created on this backend. One
  // dispatch carries every rectangle of the pair: the shader covers their
  // bounding box and keeps a word only for the samples inside a rectangle.
  bool words_failed = false;
  if (!direct) {
    RECT union_rect = rects[0];
    uint64_t rect_area = 0;
    for (uint32_t i = 0; i < rect_count; ++i) {
      union_rect.left = std::min(union_rect.left, rects[i].left);
      union_rect.top = std::min(union_rect.top, rects[i].top);
      union_rect.right = std::max(union_rect.right, rects[i].right);
      union_rect.bottom = std::max(union_rect.bottom, rects[i].bottom);
      rect_area += uint64_t(rects[i].right - rects[i].left) *
                   uint64_t(rects[i].bottom - rects[i].top);
    }
    const uint64_t union_area = uint64_t(union_rect.right - union_rect.left) *
                                uint64_t(union_rect.bottom - union_rect.top);
    // The bounding box dispatches threads that belong to no rectangle: worth it
    // while it is at most a few times the rectangles' own area, not beyond.
    const bool batch = rect_count == 1 || union_area <= rect_area * kMaxBatchUnionWaste;
    context->CSSetShader(words_shader, nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &transfer_constants_);
    ID3D11ShaderResourceView* source_views[2] = {
        source.srv, source_kind == kTransferSourceDepth ? source.stencil_srv : nullptr};
    context->CSSetShaderResources(0, 2, source_views);
    context->CSSetUnorderedAccessViews(0, 1, &transfer_words_uav_, nullptr);
    if (batch) {
      uint32_t words_constants[8 + kMaxBatchRects * 2] = {};
      words_constants[0] = rect_count;
      words_constants[1] = dest_layout;
      words_constants[2] = source_layout;
      words_constants[3] = flags;
      words_constants[4] = dest_width;
      words_constants[5] = dest.samples;
      words_constants[6] = PackRectOrigin(union_rect);
      words_constants[7] = PackRectSize(union_rect);
      for (uint32_t i = 0; i < rect_count; ++i) {
        words_constants[8 + i * 2] = PackRectOrigin(rects[i]);
        words_constants[8 + i * 2 + 1] = PackRectSize(rects[i]);
      }
      if (!set_constants(words_constants, 8 + rect_count * 2)) {
        words_failed = true;
      } else {
        context->Dispatch((uint32_t(union_rect.right - union_rect.left) + 7) / 8,
                          (uint32_t(union_rect.bottom - union_rect.top) + 7) / 8, 1);
        ++transfer_words_dispatches_;
        ++transfer_batches_;
      }
    }
    if (!batch || words_failed) {
      // The single-rectangle form (fh1_native_transfer_words.cs.hlsl): one
      // dispatch per rectangle, for a bounding box too wasteful to batch or for
      // the fh1_debug_transfer_unbatched bisect.
      ID3D11ComputeShader* single_shader =
          GetTransferWordsShader(source_kind, source.samples > 1);
      if (!single_shader) {
        words_failed = true;
      } else {
        context->CSSetShader(single_shader, nullptr, 0);
      }
    }
    if (!words_failed) {
      for (uint32_t i = 0; i < rect_count; ++i) {
        // A batched dispatch already covered every rectangle.
        if (batch) {
          break;
        }
        const RECT& rect = rects[i];
        const uint32_t words_constants[8] = {
            PackRectOrigin(rect), PackRectSize(rect), dest_layout, source_layout, flags,
            dest_width, dest.samples, 0u};
        if (!set_constants(words_constants, 8)) {
          words_failed = true;
          break;
        }
        context->Dispatch((uint32_t(rect.right - rect.left) + 7) / 8,
                          (uint32_t(rect.bottom - rect.top) + 7) / 8, 1);
        ++transfer_words_dispatches_;
      }
    }
    {
      // The words become a pixel shader resource: a UAV bind left in place would
      // make the runtime drop the SRV of the same resource.
      ID3D11UnorderedAccessView* no_uav = nullptr;
      context->CSSetUnorderedAccessViews(0, 1, &no_uav, nullptr);
      ID3D11ShaderResourceView* no_source_views[2] = {};
      context->CSSetShaderResources(0, 2, no_source_views);
      context->CSSetShader(nullptr, nullptr, 0);
    }
    if (words_failed) {
      ++transfer_skipped_format_;
      return;
    }
    transfer_word_rects_ += rect_count;
    transfer_words_ += rect_area * dest.samples;
  }

  // Second step: the depth pass writes the destination's depth, and resets its
  // stencil when the previous owner may have left one.
  // The probe is a readback, so it is bounded: eight transfers that actually
  // inherit content, over rectangles whose predecessor means the same thing in
  // the same pixels - the same pitch, sample mode and host sample count, which
  // a colour predecessor at another mode does not satisfy (its rectangle covers
  // other EDRAM samples). A colour predecessor is probed only in the first few
  // attempts (a claimant's first depth tiles usually inherit an empty colour
  // surface, which says nothing about content moving); after that the probe
  // waits for a depth predecessor, whose words hold what the draws wrote. A
  // depth predecessor is probed whatever its sample mode: its words are the
  // depth words, and the comparison is a corner of the same EDRAM range (the
  // C3 probe). A colour predecessor is probed only at the same layout and
  // sample count, where its rectangle means the same pixels. One rectangle per
  // transfer is probed - the batch's first - so a batched transfer compares the
  // same top-left rectangle C3 probed, not rectangles whose rows mean other
  // EDRAM samples in the predecessor.
  const bool same_layout = dest_key.pitch_tiles == source.key.pitch_tiles &&
                           dest_key.msaa == source.key.msaa &&
                           dest.samples == source.samples;
  int64_t probe_before = -1;
  if (REXCVAR_GET(fh1_swap_fetch_trace) && transfer_depth_verified_ < 8 &&
      transfer_depth_probe_attempts_ < 60 &&
      (source.key.is_depth || (same_layout && transfer_depth_probe_attempts_ < 12))) {
    ++transfer_depth_probe_attempts_;
    probe_before = CountEmptyDepth(dest, rects[0]);
  }
  context->OMSetRenderTargets(0, nullptr, dest.dsv);
  // 2x stored as 4x: the guest's two samples are the top-left and bottom-right
  // ones, the same mask the Direct3D 12 executor's transfer pipelines use.
  const uint32_t sample_mask =
      dest.key.msaa == uint32_t(xenos::MsaaSamples::k2X) && dest.samples == 4 ? 0b1001u
                                                                            : UINT_MAX;
  context->OMSetBlendState(nullptr, nullptr, sample_mask);
  context->OMSetDepthStencilState(GetTransferDepthPassState(stencil), 0);
  context->RSSetState(transfer_rasterizer_state_);
  context->IASetInputLayout(nullptr);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
  context->VSSetShader(transfer_vertex_shader_, nullptr, 0);
  context->PSSetShader(depth_shader, nullptr, 0);
  context->PSSetConstantBuffers(0, 1, &transfer_constants_);
  if (direct) {
    // The pass reads the previous owner's surface itself, so the constants are
    // its layout rather than the words buffer's shape.
    ID3D11ShaderResourceView* source_views[2] = {
        source.srv, source_kind == kTransferSourceDepth ? source.stencil_srv : nullptr};
    context->PSSetShaderResources(0, source_kind == kTransferSourceDepth ? 2u : 1u,
                                  source_views);
  } else {
    ID3D11ShaderResourceView* words_views[2] = {transfer_words_srv_, nullptr};
    context->PSSetShaderResources(0, 2, words_views);
  }
  const uint32_t dest_shape = dest_width | (dest.samples << 16);
  const uint32_t pass_constants[3] = {dest_layout, direct ? source_layout : dest_shape, flags};
  if (!set_constants(pass_constants, 3)) {
    ++transfer_skipped_format_;
    return;
  }
  for (uint32_t i = 0; i < rect_count; ++i) {
    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = float(rects[i].left);
    viewport.TopLeftY = float(rects[i].top);
    viewport.Width = float(rects[i].right - rects[i].left);
    viewport.Height = float(rects[i].bottom - rects[i].top);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);
    context->RSSetScissorRects(1, &rects[i]);
    context->Draw(3, 0);
  }
  ++transfer_depth_passes_;

  // Third step: one pass per stencil bit the previous owner may hold. The
  // shader discards where the bit is clear, the pipeline replaces it where it
  // is not - the Direct3D 12 executor's eight stencil passes.
  if (stencil) {
    if (stencil_shader) {
      context->PSSetShader(stencil_shader, nullptr, 0);
      for (uint32_t bit = 0; bit < 8; ++bit) {
        const uint32_t stencil_constants[3] = {
            dest_layout, direct ? source_layout : dest_shape, flags | (bit << 8)};
        if (!set_constants(stencil_constants, 3)) {
          break;
        }
        context->OMSetDepthStencilState(GetTransferStencilState(bit), 1u << bit);
        for (uint32_t i = 0; i < rect_count; ++i) {
          D3D11_VIEWPORT viewport = {};
          viewport.TopLeftX = float(rects[i].left);
          viewport.TopLeftY = float(rects[i].top);
          viewport.Width = float(rects[i].right - rects[i].left);
          viewport.Height = float(rects[i].bottom - rects[i].top);
          viewport.MinDepth = 0.0f;
          viewport.MaxDepth = 1.0f;
          context->RSSetViewports(1, &viewport);
          context->RSSetScissorRects(1, &rects[i]);
          context->Draw(3, 0);
        }
        ++transfer_stencil_passes_;
      }
    } else {
      ++transfer_skipped_format_;
    }
  }
  if (direct) {
    // The source stays bound as a pixel shader resource otherwise; a later draw
    // that writes that surface would make the runtime drop one of the two binds
    // silently. Nothing else reads it.
    ID3D11ShaderResourceView* no_source_views[2] = {};
    context->PSSetShaderResources(0, 2, no_source_views);
    ++transfer_depth_direct_;
  }
  ++transfers_;
  transfer_tiles_ += tile_count;
  if (probe_before >= 0) {
    VerifyDepthTransfer(dest, source, rects[0], uint64_t(probe_before));
  }
}

bool D3D11Fh1NativeExecutor::EnsureTransferWords(uint64_t word_count) {
  // 16M words is 64 MB: the largest depth rectangle the guest's video mode
  // can ask for at 4x (1280 x 720 x 4) fits in a quarter of it.
  constexpr uint32_t kMaxWords = 16u << 20;
  word_count = (word_count + 3u) & ~uint64_t(3);
  if (!word_count || word_count > kMaxWords) {
    return false;
  }
  if (transfer_words_uav_ && transfer_words_srv_ && transfer_words_capacity_ >= word_count) {
    return true;
  }
  ID3D11Device* device = command_processor_.GetD3D11Provider().GetDevice();
  if (!device) {
    return false;
  }
  uint32_t words = 1u << 18;
  while (words < word_count) {
    words <<= 1;
  }
  if (words > kMaxWords) {
    words = kMaxWords;
  }
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = words * 4u;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  ID3D11Buffer* buffer = nullptr;
  const HRESULT buffer_hr = device->CreateBuffer(&desc, nullptr, &buffer);
  if (FAILED(buffer_hr)) {
    REXGPU_ERROR(
        "D3D11 FH1 native executor: transfer words buffer creation failed ({} words) hr={:08X} "
        "removed_reason={:08X}",
        words, uint32_t(buffer_hr), uint32_t(device->GetDeviceRemovedReason()));
    return false;
  }
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
  uav_desc.Format = DXGI_FORMAT_R32_UINT;
  uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uav_desc.Buffer.FirstElement = 0;
  uav_desc.Buffer.NumElements = words;
  ID3D11UnorderedAccessView* uav = nullptr;
  const HRESULT uav_hr = device->CreateUnorderedAccessView(buffer, &uav_desc, &uav);
  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  srv_desc.Format = DXGI_FORMAT_R32_UINT;
  srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  srv_desc.Buffer.FirstElement = 0;
  srv_desc.Buffer.NumElements = words;
  ID3D11ShaderResourceView* srv = nullptr;
  const HRESULT srv_hr = device->CreateShaderResourceView(buffer, &srv_desc, &srv);
  if (FAILED(uav_hr) || FAILED(srv_hr)) {
    REXGPU_ERROR(
        "D3D11 FH1 native executor: transfer words views failed (uav hr={:08X} srv hr={:08X} "
        "removed_reason={:08X})",
        uint32_t(uav_hr), uint32_t(srv_hr), uint32_t(device->GetDeviceRemovedReason()));
    if (uav) uav->Release();
    if (srv) srv->Release();
    buffer->Release();
    return false;
  }
  if (transfer_words_srv_) transfer_words_srv_->Release();
  if (transfer_words_uav_) transfer_words_uav_->Release();
  if (transfer_words_buffer_) transfer_words_buffer_->Release();
  transfer_words_buffer_ = buffer;
  transfer_words_uav_ = uav;
  transfer_words_srv_ = srv;
  transfer_words_capacity_ = words;
  REXGPU_INFO("D3D11 FH1 native executor: transfer words buffer ready ({} KB, typed R32_UINT)",
              (uint64_t(words) * 4u) >> 10);
  return true;
}

ID3D11ComputeShader* D3D11Fh1NativeExecutor::GetTransferWordsShader(uint32_t source_kind,
                                                                    bool source_msaa) {
  const uint32_t key = (source_kind & 3) | (uint32_t(source_msaa) << 2);
  auto it = transfer_words_shaders_.find(key);
  if (it != transfer_words_shaders_.end()) {
    return it->second;
  }
  struct Bytecode {
    const void* data;
    size_t size;
  };
  static const Bytecode kShaders[3][2] = {
      {FH1_TRANSFER_SHADER(fh1_native_transfer_words_color_typed_cs),
       FH1_TRANSFER_SHADER(fh1_native_transfer_words_color_ms_typed_cs)},
      {FH1_TRANSFER_SHADER(fh1_native_transfer_words_depth_typed_cs),
       FH1_TRANSFER_SHADER(fh1_native_transfer_words_depth_ms_typed_cs)},
      {FH1_TRANSFER_SHADER(fh1_native_transfer_words_uint_typed_cs),
       FH1_TRANSFER_SHADER(fh1_native_transfer_words_uint_ms_typed_cs)}};
  const Bytecode& bytecode = kShaders[std::min(source_kind, 2u)][source_msaa ? 1 : 0];
  ID3D11Device* device = command_processor_.GetD3D11Provider().GetDevice();
  ID3D11ComputeShader* shader = nullptr;
  if (!device || FAILED(device->CreateComputeShader(bytecode.data, bytecode.size, nullptr,
                                                    &shader))) {
    shader = nullptr;
    REXGPU_ERROR("D3D11 FH1 native executor: transfer words shader creation failed (key {})",
                 key);
  }
  transfer_words_shaders_.emplace(key, shader);
  return shader;
}

ID3D11ComputeShader* D3D11Fh1NativeExecutor::GetTransferWordsBatchShader(uint32_t source_kind,
                                                                        bool source_msaa) {
  const uint32_t key = (source_kind & 3) | (uint32_t(source_msaa) << 2);
  auto it = transfer_words_batch_shaders_.find(key);
  if (it != transfer_words_batch_shaders_.end()) {
    return it->second;
  }
  struct Bytecode {
    const void* data;
    size_t size;
  };
  static const Bytecode kShaders[3][2] = {
      {FH1_TRANSFER_SHADER(fh1_native_transfer_words_batch_color_typed_cs),
       FH1_TRANSFER_SHADER(fh1_native_transfer_words_batch_color_ms_typed_cs)},
      {FH1_TRANSFER_SHADER(fh1_native_transfer_words_batch_depth_typed_cs),
       FH1_TRANSFER_SHADER(fh1_native_transfer_words_batch_depth_ms_typed_cs)},
      {FH1_TRANSFER_SHADER(fh1_native_transfer_words_batch_uint_typed_cs),
       FH1_TRANSFER_SHADER(fh1_native_transfer_words_batch_uint_ms_typed_cs)}};
  const Bytecode& bytecode = kShaders[std::min(source_kind, 2u)][source_msaa ? 1 : 0];
  ID3D11Device* device = command_processor_.GetD3D11Provider().GetDevice();
  ID3D11ComputeShader* shader = nullptr;
  if (!device || FAILED(device->CreateComputeShader(bytecode.data, bytecode.size, nullptr,
                                                    &shader))) {
    shader = nullptr;
    REXGPU_ERROR(
        "D3D11 FH1 native executor: batched transfer words shader creation failed (key {})", key);
  }
  transfer_words_batch_shaders_.emplace(key, shader);
  return shader;
}

ID3D11PixelShader* D3D11Fh1NativeExecutor::GetTransferDirectShader(uint32_t source_kind,
                                                                  bool source_msaa,
                                                                  uint32_t dest_kind,
                                                                  bool dest_msaa) {
  const uint32_t key = TransferShaderKey(source_kind, source_msaa, dest_kind, dest_msaa);
  auto it = transfer_direct_shaders_.find(key);
  if (it != transfer_direct_shaders_.end()) {
    return it->second;
  }
  struct Bytecode {
    const void* data;
    size_t size;
  };
  // [source kind][source MSAA][destination MSAA].
  static const Bytecode kDepthShaders[3][2][2] = {
      {{FH1_TRANSFER_SHADER(fh1_native_transfer_depth_from_color_ps),
        FH1_TRANSFER_SHADER(fh1_native_transfer_depth_dms_from_color_ps)},
       {FH1_TRANSFER_SHADER(fh1_native_transfer_depth_from_color_ms_ps),
        FH1_TRANSFER_SHADER(fh1_native_transfer_depth_dms_from_color_ms_ps)}},
      {{FH1_TRANSFER_SHADER(fh1_native_transfer_depth_from_depth_ps),
        FH1_TRANSFER_SHADER(fh1_native_transfer_depth_dms_from_depth_ps)},
       {FH1_TRANSFER_SHADER(fh1_native_transfer_depth_from_depth_ms_ps),
        FH1_TRANSFER_SHADER(fh1_native_transfer_depth_dms_from_depth_ms_ps)}},
      {{FH1_TRANSFER_SHADER(fh1_native_transfer_depth_from_uint_ps),
        FH1_TRANSFER_SHADER(fh1_native_transfer_depth_dms_from_uint_ps)},
       {FH1_TRANSFER_SHADER(fh1_native_transfer_depth_from_uint_ms_ps),
        FH1_TRANSFER_SHADER(fh1_native_transfer_depth_dms_from_uint_ms_ps)}}};
  static const Bytecode kStencilShaders[3][2][2] = {
      {{FH1_TRANSFER_SHADER(fh1_native_transfer_stencil_from_color_ps),
        FH1_TRANSFER_SHADER(fh1_native_transfer_stencil_dms_from_color_ps)},
       {FH1_TRANSFER_SHADER(fh1_native_transfer_stencil_from_color_ms_ps),
        FH1_TRANSFER_SHADER(fh1_native_transfer_stencil_dms_from_color_ms_ps)}},
      {{FH1_TRANSFER_SHADER(fh1_native_transfer_stencil_from_depth_ps),
        FH1_TRANSFER_SHADER(fh1_native_transfer_stencil_dms_from_depth_ps)},
       {FH1_TRANSFER_SHADER(fh1_native_transfer_stencil_from_depth_ms_ps),
        FH1_TRANSFER_SHADER(fh1_native_transfer_stencil_dms_from_depth_ms_ps)}},
      {{FH1_TRANSFER_SHADER(fh1_native_transfer_stencil_from_uint_ps),
        FH1_TRANSFER_SHADER(fh1_native_transfer_stencil_dms_from_uint_ps)},
       {FH1_TRANSFER_SHADER(fh1_native_transfer_stencil_from_uint_ms_ps),
        FH1_TRANSFER_SHADER(fh1_native_transfer_stencil_dms_from_uint_ms_ps)}}};
  const Bytecode& bytecode =
      (dest_kind == kTransferDestStencil ? kStencilShaders : kDepthShaders)
          [std::min(source_kind, 2u)][source_msaa ? 1 : 0][dest_msaa ? 1 : 0];
  ID3D11Device* device = command_processor_.GetD3D11Provider().GetDevice();
  ID3D11PixelShader* shader = nullptr;
  if (!device ||
      FAILED(device->CreatePixelShader(bytecode.data, bytecode.size, nullptr, &shader))) {
    shader = nullptr;
    REXGPU_ERROR(
        "D3D11 FH1 native executor: direct transfer pixel shader creation failed (key {})", key);
  }
  transfer_direct_shaders_.emplace(key, shader);
  return shader;
}

ID3D11PixelShader* D3D11Fh1NativeExecutor::GetTransferFromWordsShader(uint32_t dest_kind,
                                                                      bool dest_msaa) {
  const uint32_t key = (dest_kind & 3) | (uint32_t(dest_msaa) << 2);
  auto it = transfer_from_words_shaders_.find(key);
  if (it != transfer_from_words_shaders_.end()) {
    return it->second;
  }
  struct Bytecode {
    const void* data;
    size_t size;
  };
  static const Bytecode kShaders[2][2] = {
      {FH1_TRANSFER_SHADER(fh1_native_transfer_depth_from_words_typed_ps),
       FH1_TRANSFER_SHADER(fh1_native_transfer_depth_dms_from_words_typed_ps)},
      {FH1_TRANSFER_SHADER(fh1_native_transfer_stencil_from_words_typed_ps),
       FH1_TRANSFER_SHADER(fh1_native_transfer_stencil_dms_from_words_typed_ps)}};
  const Bytecode& bytecode = kShaders[dest_kind == kTransferDestStencil ? 1 : 0]
                                     [dest_msaa ? 1 : 0];
  ID3D11Device* device = command_processor_.GetD3D11Provider().GetDevice();
  ID3D11PixelShader* shader = nullptr;
  if (!device ||
      FAILED(device->CreatePixelShader(bytecode.data, bytecode.size, nullptr, &shader))) {
    shader = nullptr;
    REXGPU_ERROR(
        "D3D11 FH1 native executor: from-words pixel shader creation failed (key {})", key);
  }
  transfer_from_words_shaders_.emplace(key, shader);
  return shader;
}

ID3D11DepthStencilState* D3D11Fh1NativeExecutor::GetTransferDepthPassState(bool reset_stencil) {
  const uint32_t key = reset_stencil ? 1u : 0u;
  auto it = transfer_depth_states_.find(key);
  if (it != transfer_depth_states_.end()) {
    return it->second;
  }
  ID3D11Device* device = command_processor_.GetD3D11Provider().GetDevice();
  D3D11_DEPTH_STENCIL_DESC desc = {};
  // The pass writes SV_Depth unconditionally, and - when the previous owner may
  // have left a stencil - replaces every stencil bit with 0 in the same pass
  // (the Direct3D 12 executor's depth pass does the same).
  desc.DepthEnable = TRUE;
  desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
  desc.StencilEnable = reset_stencil ? TRUE : FALSE;
  if (reset_stencil) {
    desc.StencilReadMask = 0;
    desc.StencilWriteMask = 0xFF;
    desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    desc.BackFace = desc.FrontFace;
  }
  ID3D11DepthStencilState* state = nullptr;
  if (!device || FAILED(device->CreateDepthStencilState(&desc, &state))) {
    state = nullptr;
    REXGPU_ERROR("D3D11 FH1 native executor: transfer depth state creation failed (key {})",
                 key);
    return nullptr;
  }
  transfer_depth_states_.emplace(key, state);
  return state;
}

ID3D11DepthStencilState* D3D11Fh1NativeExecutor::GetTransferStencilState(uint32_t bit) {
  const uint32_t key = 0x100u | (bit & 7u);
  auto it = transfer_depth_states_.find(key);
  if (it != transfer_depth_states_.end()) {
    return it->second;
  }
  ID3D11Device* device = command_processor_.GetD3D11Provider().GetDevice();
  D3D11_DEPTH_STENCIL_DESC desc = {};
  // Stencil only: depth is kept (the depth pass wrote it), the shader discarded
  // the pixels whose bit is clear, the pipeline replaces this one bit.
  desc.DepthEnable = TRUE;
  desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
  desc.StencilEnable = TRUE;
  desc.StencilReadMask = 0;
  desc.StencilWriteMask = uint8_t(1u << (bit & 7u));
  desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
  desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
  desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
  desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
  desc.BackFace = desc.FrontFace;
  ID3D11DepthStencilState* state = nullptr;
  if (!device || FAILED(device->CreateDepthStencilState(&desc, &state))) {
    state = nullptr;
    REXGPU_ERROR("D3D11 FH1 native executor: transfer stencil state creation failed (bit {})",
                 bit);
    return nullptr;
  }
  transfer_depth_states_.emplace(key, state);
  return state;
}

int64_t D3D11Fh1NativeExecutor::CountEmptyDepth(const D3D11Fh1NativeSurface& surface,
                                                const RECT& rect) {
  const LONG width = rect.right - rect.left, height = rect.bottom - rect.top;
  ID3D11Device* device = command_processor_.GetD3D11Provider().GetDevice();
  ID3D11DeviceContext* context = command_processor_.GetD3D11Provider().GetImmediateContext();
  static uint32_t probe_failures = 0;
  auto fail = [&](const char* reason, HRESULT hr) -> int64_t {
    if (probe_failures < 6) {
      ++probe_failures;
      REXGPU_INFO(
          "D3D11 FH1 native executor: depth probe unavailable ({}) hr={:08X} removed_reason={:08X}",
          reason, uint32_t(hr), device ? uint32_t(device->GetDeviceRemovedReason()) : 0u);
    }
    return -1;
  };
  if (!device || !context) {
    return fail("no device", E_FAIL);
  }
  if (!surface.key.is_depth || !surface.srv || width <= 0 || height <= 0) {
    if (probe_failures < 6) {
      ++probe_failures;
      REXGPU_INFO(
          "D3D11 FH1 native executor: depth probe unavailable (surface not probeable: {} depth={} "
          "srv={} samples={} rect={},{}-{},{})",
          surface.key.Describe(), surface.key.is_depth, surface.srv ? 1 : 0, surface.samples,
          rect.left, rect.top, rect.right, rect.bottom);
    }
    return -1;
  }
  if (!EnsureTransferResources()) {
    return fail("transfer resources", E_FAIL);
  }
  ID3D11PixelShader** shader_slot =
      surface.samples > 1 ? &depth_probe_shader_ms_ : &depth_probe_shader_;
  if (!*shader_slot) {
    const bool msaa = surface.samples > 1;
    const HRESULT shader_hr = device->CreatePixelShader(
        msaa ? shaders::fh1_native_depth_probe_ms_ps : shaders::fh1_native_depth_probe_ps,
        msaa ? sizeof(shaders::fh1_native_depth_probe_ms_ps)
             : sizeof(shaders::fh1_native_depth_probe_ps),
        nullptr, shader_slot);
    if (FAILED(shader_hr)) {
      *shader_slot = nullptr;
      return fail("probe pixel shader", shader_hr);
    }
  }
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = UINT(width);
  desc.Height = UINT(height);
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R32_FLOAT;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> target;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_view;
  const HRESULT target_hr = device->CreateTexture2D(&desc, nullptr, &target);
  if (FAILED(target_hr)) {
    return fail("probe target texture", target_hr);
  }
  const HRESULT view_hr = device->CreateRenderTargetView(target.Get(), nullptr, &target_view);
  if (FAILED(view_hr)) {
    return fail("probe target view", view_hr);
  }
  D3D11_TEXTURE2D_DESC staging_desc = desc;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  const HRESULT staging_hr = device->CreateTexture2D(&staging_desc, nullptr, &staging);
  if (FAILED(staging_hr)) {
    return fail("probe staging texture", staging_hr);
  }
  // Binding this target unbinds the surface's DSV, so the surface's own view
  // below is not dropped as a shader resource.
  context->OMSetRenderTargets(1, target_view.GetAddressOf(), nullptr);
  context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  context->OMSetDepthStencilState(transfer_depth_stencil_state_, 0);
  context->RSSetState(transfer_rasterizer_state_);
  D3D11_VIEWPORT viewport = {};
  viewport.Width = float(width);
  viewport.Height = float(height);
  viewport.MaxDepth = 1.0f;
  context->RSSetViewports(1, &viewport);
  const RECT scissor = {0, 0, width, height};
  context->RSSetScissorRects(1, &scissor);
  context->IASetInputLayout(nullptr);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
  context->VSSetShader(transfer_vertex_shader_, nullptr, 0);
  context->PSSetShader(*shader_slot, nullptr, 0);
  D3D11_MAPPED_SUBRESOURCE mapped_constants;
  const HRESULT constants_hr =
      context->Map(transfer_constants_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_constants);
  if (FAILED(constants_hr)) {
    return fail("probe constants map", constants_hr);
  }
  const uint32_t origin[4] = {uint32_t(rect.left), uint32_t(rect.top), surface.samples, 0u};
  std::memcpy(mapped_constants.pData, origin, sizeof(origin));
  context->Unmap(transfer_constants_, 0);
  context->PSSetConstantBuffers(0, 1, &transfer_constants_);
  ID3D11ShaderResourceView* views[2] = {surface.srv, nullptr};
  context->PSSetShaderResources(0, 2, views);
  context->Draw(3, 0);
  ID3D11ShaderResourceView* no_views[2] = {};
  context->PSSetShaderResources(0, 2, no_views);
  context->CopyResource(staging.Get(), target.Get());
  D3D11_MAPPED_SUBRESOURCE mapped;
  const HRESULT read_hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(read_hr)) {
    return fail("probe staging map", read_hr);
  }
  int64_t empty = 0;
  for (LONG y = 0; y < height; ++y) {
    const float* row = reinterpret_cast<const float*>(
        static_cast<const uint8_t*>(mapped.pData) + size_t(mapped.RowPitch) * y);
    for (LONG x = 0; x < width; ++x) {
      empty += row[x] == 0.0f;
    }
  }
  context->Unmap(staging.Get(), 0);
  return empty;
}

void D3D11Fh1NativeExecutor::VerifyDepthTransfer(D3D11Fh1NativeSurface& dest,
                                                 D3D11Fh1NativeSurface& source,
                                                 const RECT& rect, uint64_t dest_before) {
  // The two rectangles are the same pixel rectangle, which only covers the same
  // EDRAM samples when the layouts match; otherwise the comparison is coarse
  // and is labelled as such rather than reported as a defect.
  const bool same_layout_probe = dest.key.pitch_tiles == source.key.pitch_tiles &&
                                 dest.key.msaa == source.key.msaa &&
                                 dest.samples == source.samples;
  const int64_t dest_after = CountEmptyDepth(dest, rect);
  // A depth claimant's predecessor may be a colour surface holding the same
  // EDRAM words: its texel is empty when its whole word is zero (the depth bits
  // and the stencil bits alike), which CountZeroTexels counts.
  const int64_t source_empty = source.key.is_depth ? CountEmptyDepth(source, rect)
                                                   : CountZeroTexels(source.texture, rect);
  if (dest_after < 0 || source_empty < 0) {
    return;
  }
  const uint64_t texels =
      uint64_t(rect.right - rect.left) * uint64_t(rect.bottom - rect.top);
  const uint64_t tolerance = std::max<uint64_t>(1, texels / 100);
  const bool faithful = uint64_t(std::llabs(dest_after - source_empty)) <= tolerance;
  // A predecessor whose whole rectangle is empty inherits emptiness: real, but
  // not evidence that content moved, so it does not use one of the eight
  // informative probes. A colour predecessor's words are colour: only bits 8:31
  // of its word reach the depth, so its texel emptiness is not comparable with
  // the depth's unless the comparison agrees anyway - it is logged as what it
  // is and never counts as evidence otherwise.
  const bool inherited_content =
      (source.key.is_depth || faithful) &&
      (uint64_t(source_empty) < texels || dest_after != int64_t(dest_before));
  if (inherited_content) {
    ++transfer_depth_verified_;
  }
  static uint32_t empty_predecessor_logs = 0;
  if (!inherited_content && ++empty_predecessor_logs > 2) {
    return;
  }
  REXGPU_INFO(
      "D3D11 FH1 native executor: depth transfer verify {} -> {} rect={},{}-{},{} texels={} "
      "empty_texels dest_before={} dest_after={} source={} ({})",
      source.key.Describe(), dest.key.Describe(), rect.left, rect.top, rect.right, rect.bottom,
      texels, dest_before, uint64_t(dest_after), uint64_t(source_empty),
      faithful ? (inherited_content ? "the claimant's depth now holds its predecessor's words"
                                    : "empty predecessor inherited")
               : (source.key.is_depth
                      ? (same_layout_probe
                             ? "MISMATCH"
                             : "coarse: the predecessor's rectangle covers other EDRAM samples")
                      : "colour predecessor: only the upper word bits reach the depth"));
}

bool D3D11Fh1NativeExecutor::EnsureTransferResources() {
  if (transfer_vertex_shader_ && transfer_constants_) {
    return true;
  }
  ID3D11Device* device = command_processor_.GetD3D11Provider().GetDevice();
  if (!device) {
    return false;
  }
  if (!transfer_vertex_shader_ &&
      FAILED(device->CreateVertexShader(shaders::fullscreen_cw_vs,
                                        sizeof(shaders::fullscreen_cw_vs), nullptr,
                                        &transfer_vertex_shader_))) {
    transfer_vertex_shader_ = nullptr;
    REXGPU_ERROR("D3D11 FH1 native executor: transfer vertex shader creation failed");
    return false;
  }
  if (!transfer_constants_) {
    D3D11_BUFFER_DESC buffer = {};
    // Four packed layouts for the ownership transfer pixel shaders (16 bytes
    // used), eight words for the words pass, and the batched words pass's eight
    // header words plus two dwords per rectangle. Everything but the batch is
    // written with WRITE_DISCARD, so the size costs nothing but a copy.
    buffer.ByteWidth = (8 + kMaxBatchRects * 2) * 4;
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &transfer_constants_))) {
      transfer_constants_ = nullptr;
      REXGPU_ERROR("D3D11 FH1 native executor: transfer constants buffer creation failed");
      return false;
    }
  }
  if (!transfer_rasterizer_state_) {
    D3D11_RASTERIZER_DESC rasterizer = {};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = FALSE;
    if (FAILED(device->CreateRasterizerState(&rasterizer, &transfer_rasterizer_state_))) {
      transfer_rasterizer_state_ = nullptr;
      REXGPU_ERROR("D3D11 FH1 native executor: transfer rasterizer state creation failed");
      return false;
    }
  }
  if (!transfer_depth_stencil_state_) {
    D3D11_DEPTH_STENCIL_DESC depth_stencil = {};
    depth_stencil.DepthEnable = FALSE;
    depth_stencil.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth_stencil.DepthFunc = D3D11_COMPARISON_ALWAYS;
    depth_stencil.StencilEnable = FALSE;
    if (FAILED(
            device->CreateDepthStencilState(&depth_stencil, &transfer_depth_stencil_state_))) {
      transfer_depth_stencil_state_ = nullptr;
      REXGPU_ERROR("D3D11 FH1 native executor: transfer depth-stencil state creation failed");
      return false;
    }
  }
  return transfer_vertex_shader_ && transfer_constants_ && transfer_rasterizer_state_ &&
         transfer_depth_stencil_state_;
}

ID3D11PixelShader* D3D11Fh1NativeExecutor::GetTransferShader(uint32_t source_kind,
                                                             bool source_msaa, uint32_t dest_kind,
                                                             bool dest_msaa) {
  const uint32_t key = TransferShaderKey(source_kind, source_msaa, dest_kind, dest_msaa);
  auto it = transfer_pixel_shaders_.find(key);
  if (it != transfer_pixel_shaders_.end()) {
    return it->second;
  }
  struct Bytecode {
    const void* data;
    size_t size;
  };
  // [raw channel bits destination][source kind][source MSAA][destination MSAA]
  static const Bytecode kShaders[2][3][2][2] = {
      // Color destination through the surface's own (float) render target.
      {{{FH1_TRANSFER_SHADER(fh1_native_transfer_color_from_color_ps),
         FH1_TRANSFER_SHADER(fh1_native_transfer_color_from_color_ms_ps)},
        {FH1_TRANSFER_SHADER(fh1_native_transfer_color_dms_from_color_ps),
         FH1_TRANSFER_SHADER(fh1_native_transfer_color_dms_from_color_ms_ps)}},
       {{FH1_TRANSFER_SHADER(fh1_native_transfer_color_from_depth_ps),
         FH1_TRANSFER_SHADER(fh1_native_transfer_color_from_depth_ms_ps)},
        {FH1_TRANSFER_SHADER(fh1_native_transfer_color_dms_from_depth_ps),
         FH1_TRANSFER_SHADER(fh1_native_transfer_color_dms_from_depth_ms_ps)}},
       {{FH1_TRANSFER_SHADER(fh1_native_transfer_color_from_uint_ps),
         FH1_TRANSFER_SHADER(fh1_native_transfer_color_from_uint_ms_ps)},
        {FH1_TRANSFER_SHADER(fh1_native_transfer_color_dms_from_uint_ps),
         FH1_TRANSFER_SHADER(fh1_native_transfer_color_dms_from_uint_ms_ps)}}},
      // Raw channel bits destination through the surface's UINT render target.
      {{{FH1_TRANSFER_SHADER(fh1_native_transfer_uint_from_color_ps),
         FH1_TRANSFER_SHADER(fh1_native_transfer_uint_from_color_ms_ps)},
        {FH1_TRANSFER_SHADER(fh1_native_transfer_uint_dms_from_color_ps),
         FH1_TRANSFER_SHADER(fh1_native_transfer_uint_dms_from_color_ms_ps)}},
       {{FH1_TRANSFER_SHADER(fh1_native_transfer_uint_from_depth_ps),
         FH1_TRANSFER_SHADER(fh1_native_transfer_uint_from_depth_ms_ps)},
        {FH1_TRANSFER_SHADER(fh1_native_transfer_uint_dms_from_depth_ps),
         FH1_TRANSFER_SHADER(fh1_native_transfer_uint_dms_from_depth_ms_ps)}},
       {{FH1_TRANSFER_SHADER(fh1_native_transfer_uint_from_uint_ps),
         FH1_TRANSFER_SHADER(fh1_native_transfer_uint_from_uint_ms_ps)},
        {FH1_TRANSFER_SHADER(fh1_native_transfer_uint_dms_from_uint_ps),
         FH1_TRANSFER_SHADER(fh1_native_transfer_uint_dms_from_uint_ms_ps)}}}};
  const Bytecode& bytecode =
      kShaders[dest_kind == kTransferDestUint ? 1 : 0][std::min(source_kind, 2u)]
               [source_msaa ? 1 : 0][dest_msaa ? 1 : 0];
  ID3D11Device* device = command_processor_.GetD3D11Provider().GetDevice();
  ID3D11PixelShader* pixel_shader = nullptr;
  if (!device || FAILED(device->CreatePixelShader(bytecode.data, bytecode.size, nullptr,
                                                  &pixel_shader))) {
    pixel_shader = nullptr;
    REXGPU_ERROR("D3D11 FH1 native executor: transfer pixel shader creation failed (key {})", key);
  }
  transfer_pixel_shaders_.emplace(key, pixel_shader);
  return pixel_shader;
}

bool D3D11Fh1NativeExecutor::BindTargets(
    uint32_t& bound_depth_and_color_render_target_bits_out,
    uint32_t* bound_depth_and_color_render_target_formats_out) {
  bound_depth_and_color_render_target_bits_out = 0;
  if (!initialized_ || !pending_targets_valid_) {
    return false;
  }
  ++binds_;
  ID3D11RenderTargetView* rtvs[xenos::kMaxColorRenderTargets] = {};
  uint32_t rtv_count = 0;
  ID3D11DepthStencilView* dsv = nullptr;
  for (uint32_t i = 1; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (!(pending_used_bits_ & (1u << i))) {
      continue;
    }
    D3D11Fh1NativeSurface* surface = pending_surfaces_[i];
    if (!surface || !surface->rtv) {
      continue;
    }
    while (rtv_count < i - 1) {
      rtvs[rtv_count++] = nullptr;
    }
    rtvs[rtv_count++] = surface->rtv;
    if (bound_depth_and_color_render_target_formats_out) {
      bound_depth_and_color_render_target_formats_out[i] = pending_keys_[i].format;
    }
    bound_depth_and_color_render_target_bits_out |= 1u << i;
  }
  if (pending_used_bits_ & 1) {
    D3D11Fh1NativeSurface* surface = pending_surfaces_[0];
    if (surface && surface->dsv) {
      dsv = surface->dsv;
      if (bound_depth_and_color_render_target_formats_out) {
        bound_depth_and_color_render_target_formats_out[0] = pending_keys_[0].format;
      }
      bound_depth_and_color_render_target_bits_out |= 1;
    }
  }
  if (!bound_depth_and_color_render_target_bits_out) {
    // Nothing to bind (no target this draw writes): the draw proceeds as it
    // does without the executor, with no targets bound.
    ++skipped_draws_;
    return true;
  }
  command_processor_.GetD3D11Provider().GetImmediateContext()->OMSetRenderTargets(
      rtv_count, rtv_count ? rtvs : nullptr, dsv);
  return true;
}

uint32_t D3D11Fh1NativeExecutor::ResolveSourceOwner(const Fh1SurfaceKey& key, int32_t x0,
                                                    int32_t y0, int32_t x1, int32_t y1) const {
  if (!initialized_ || !key.pitch_tiles || x0 >= x1 || y0 >= y1) {
    return Fh1EdramTiles::kNoOwner;
  }
  const uint32_t msaa_x_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k4X));
  const uint32_t msaa_y_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k2X));
  const int32_t tile_width = int32_t(xenos::kEdramTileWidthSamples >> msaa_x_log2);
  const int32_t tile_height = int32_t(xenos::kEdramTileHeightSamples >> msaa_y_log2);
  const int32_t column_first = x0 / tile_width,
                column_end = (x1 + tile_width - 1) / tile_width;
  const int32_t row_first = y0 / tile_height, row_end = (y1 + tile_height - 1) / tile_height;
  // fh1_debug_resolve_owner_census: what the copy's rectangle is made of in
  // the EDRAM tile table at the moment the resolve plans its source. The tiles
  // being unowned means no draw claimed them since the table was reset; being
  // mixed (or a different MSAA/depth class) is what sends the by-owner lookup
  // back to the copy's own surface even when another surface holds the words.
  const int32_t census_budget = REXCVAR_GET(fh1_debug_resolve_owner_census);
  static uint32_t census_logs = 0;
  const bool census = census_budget > 0 && census_logs < uint32_t(census_budget) &&
                      key.base_tiles != 0 && key.pitch_tiles == 16 && !key.is_depth &&
                      key.msaa == uint32_t(xenos::MsaaSamples::k1X);
  uint32_t census_unowned = 0;
  std::map<uint32_t, uint32_t> census_owners;
  uint32_t owner = Fh1EdramTiles::kNoOwner;
  bool mixed = false;
  for (int32_t row = row_first; row < row_end; ++row) {
    for (int32_t column = column_first; column < column_end; ++column) {
      const uint32_t tile_owner = tiles_.Owner(key.base_tiles +
                                               uint32_t(row) * key.pitch_tiles +
                                               uint32_t(column));
      if (tile_owner == Fh1EdramTiles::kNoOwner) {
        // Never claimed since Reset: nothing was written, and the surface the
        // copy names is as good as this one for those tiles.
        if (census) ++census_unowned;
        continue;
      }
      if (census) ++census_owners[tile_owner];
      if (owner == Fh1EdramTiles::kNoOwner) {
        owner = tile_owner;
      } else if (tile_owner != owner) {
        // Part of the rectangle was written by another surface: C2's per-owner
        // runs split the read; until then the copy's own key is the safe
        // answer rather than a rect that silently mixes two surfaces' words.
        mixed = true;
        break;
      }
    }
    if (mixed) break;
  }
  // The sample mode and depth-ness are part of the data layout, not a format
  // detail: a 4x surface at half the pitch covers the same EDRAM tiles as a 1x
  // surface but holds samples, not pixels, so it is not that copy's source.
  constexpr uint32_t kMsaaDepthMask = (0x3u << 19) | (1u << 21);
  const bool class_mismatch =
      owner != Fh1EdramTiles::kNoOwner && (owner & kMsaaDepthMask) != (key.Pack() & kMsaaDepthMask);
  if (census) {
    ++census_logs;
    auto describe_owner = [](uint32_t packed) {
      Fh1SurfaceKey decoded;
      decoded.base_tiles = packed & 0x7FF;
      decoded.pitch_tiles = (packed >> 11) & 0xFF;
      decoded.msaa = (packed >> 19) & 3;
      decoded.is_depth = ((packed >> 21) & 1) != 0;
      decoded.format = packed >> 22;
      return decoded.Describe();
    };
    std::string histogram;
    for (const auto& entry : census_owners) {
      histogram += " " + describe_owner(entry.first) + ":" + std::to_string(entry.second);
    }
    REXGPU_INFO(
        "FH1 resolve owner census: key={} rect={},{}-{},{} tiles={} unowned={} owners={} "
        "mixed={} class={} result={}",
        key.Describe(), x0, y0, x1, y1,
        uint32_t(row_end - row_first) * uint32_t(column_end - column_first), census_unowned,
        histogram.empty() ? "none" : histogram.c_str(), mixed ? 1 : 0, class_mismatch ? 1 : 0,
        mixed || class_mismatch || owner == Fh1EdramTiles::kNoOwner ? "none"
                                                                   : describe_owner(owner));
  }
  if (mixed || class_mismatch) {
    return Fh1EdramTiles::kNoOwner;
  }
  return owner;
}

void D3D11Fh1NativeExecutor::NativeDrawIssued(const Fh1DrawInfo& draw) {
  if (!initialized_) {
    return;
  }
  if (draw.memexport) {
    return;
  }
  ++draws_;
}

void D3D11Fh1NativeExecutor::OnSwap(uint64_t frame, uint32_t frontbuffer_address) {
  if (!initialized_) {
    return;
  }
  (void)frontbuffer_address;
  ++frames_;
  // Periodic line so a session that hard-exits at the title still reports the
  // executor's work (the shutdown summary does not always run).
  if (frame != last_stats_frame_ && (frame % 100) == 0) {
    last_stats_frame_ = frame;
    LogStats(frame);
  }
}

void D3D11Fh1NativeExecutor::LogStats(uint64_t frame) {
  REXGPU_INFO(
      "D3D11 FH1 native executor: frame={} prepares={} binds={} draws={} skipped={} claims={} "
      "tiles={} transfers={} transfer_tiles={} (draws={} copies={}) "
      "transfer_skipped={}/{}/{} words={} word_rects={} words_written={} batches={} "
      "depth_direct={} depth_passes={} stencil_passes={} "
      "depth_verified={} owner_resolves={} surfaces={} owner_gen={}",
      frame, prepares_, binds_, draws_, skipped_draws_, claims_, claimed_tiles_, transfers_,
      transfer_tiles_, transfer_draws_, transfer_copies_, transfer_skipped_source_,
      transfer_skipped_format_, transfer_skipped_disabled_, transfer_words_dispatches_,
      transfer_word_rects_, transfer_words_, transfer_batches_, transfer_depth_direct_,
      transfer_depth_passes_, transfer_stencil_passes_, transfer_depth_verified_,
      owner_resolves_,
      config_.surfaces ? config_.surfaces->surface_count() : 0,
      static_cast<uint64_t>(tiles_.generation()));
}

}  // namespace rex::graphics::d3d11

#undef FH1_TRANSFER_SHADER
