/**
 * @file        graphics/d3d11/texture_cache.cpp
 * @brief       D3D11 backend texture cache (Phase 2: CPU untiling + uploads)
 *
 *              The Direct3D 12 backend converts guest texture memory to host
 *              images on the GPU with load shaders. Until the Direct3D 11
 *              compute load shaders land (Phase 4), this backend untiles and
 *              converts on the CPU: SharedMemory::CopyCpuRange makes the guest
 *              base/mip spans CPU-authoritative, and the device-free
 *              texture_conversion::CopyGuestLevelSlice driver gathers guest
 *              block runs (tiled or linear, 2D or 3D, packed mip tails) into a
 *              row-major staging image through per-format CPU converters,
 *              uploaded with UpdateSubresource.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/texture_cache.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstring>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/graphics/d3d11/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/d3d11/d3d11_api.h>

namespace rex::graphics::d3d11 {

namespace {

using texture_conversion::ConvertRunCallback;
using texture_conversion::ConvertRunCopy;
using texture_conversion::ConvertRunCTX1ToR8G8;
using texture_conversion::ConvertRunDepth24FloatToR32;
using texture_conversion::ConvertRunDepth24UnormToR32;
using texture_conversion::ConvertRunDXT3AToR8;
using texture_conversion::ConvertRunR5G5B5A1ToB5G5R5A1;
using texture_conversion::ConvertRunR5G6B5ToB5G6R5;
using texture_conversion::ConvertRunRGBA4ToBGRA4;

// Host representation of one guest texture format, indexed by
// xenos::TextureFormat. The DXGI mappings and swizzles are ported from the
// Direct3D 12 backend's table (same host formats, so textures of both
// backends match); the load shader columns are replaced by the CPU converter
// of the Phase 2 load path. Formats whose converter is null are not supported
// yet - CreateTexture rejects them until their conversion lands.
struct HostFormat {
  // DXGI format (typeless when different signedness is used) of the resource.
  DXGI_FORMAT dxgi_format_resource;
  // DXGI format for unsigned normalized or unsigned/signed float SRV.
  DXGI_FORMAT dxgi_format_unsigned;
  // DXGI format for signed normalized SRV.
  DXGI_FORMAT dxgi_format_signed;
  // Whether the DXGI format consists of blocks the same size as the guest's.
  bool is_block_compressed;
  // Uncompression target for block-compressed formats with unaligned sizes
  // (not supported by the CPU load path currently).
  DXGI_FORMAT dxgi_format_uncompressed;
  // Mapping of Xenos swizzle components to DXGI format components.
  uint32_t swizzle;
  // CPU guest block -> host unit conversion. Null: not supported yet.
  ConvertRunCallback convert;
  // Host rows written per guest block row (1 unless the host unit is a 1x1
  // texel of a compressed guest block).
  uint32_t convert_rows;
  // Distance between the outputs of two adjacent guest blocks within a host
  // row (the host unit's width along X), in bytes.
  uint32_t convert_block_row_bytes;
};

constexpr HostFormat kHostFormats[64] = {
    // k_1_REVERSE
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, nullptr, 1, 1},
    // k_1
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, nullptr, 1, 1},
    // k_8
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, ConvertRunCopy, 1, 1},
    // k_1_5_5_5
    // Red and blue swapped in the converter for simplicity.
    {DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_UNKNOWN, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, ConvertRunR5G5B5A1ToB5G5R5A1, 1, 2},
    // k_5_6_5
    // Red and blue swapped in the converter for simplicity.
    {DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_UNKNOWN, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB, ConvertRunR5G6B5ToB5G6R5, 1, 2},
    // k_6_5_5
    // Needs the R5G5B6ToB5G6R5 converter with the RBGA swizzle - unsupported
    // on the CPU load path for now.
    {DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_UNKNOWN, false,
     DXGI_FORMAT_UNKNOWN, XE_GPU_MAKE_TEXTURE_SWIZZLE(R, B, G, G), nullptr, 1, 2},
    // k_8_8_8_8
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, ConvertRunCopy, 1, 4},
    // k_2_10_10_10
    {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_UNKNOWN, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, ConvertRunCopy, 1, 4},
    // k_8_A
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, ConvertRunCopy, 1, 1},
    // k_8_B
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, nullptr, 1, 1},
    // k_8_8
    {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_SNORM, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG, ConvertRunCopy, 1, 2},
    // k_Cr_Y1_Cb_Y0_REP
    // 4:2:2 subsampled - needs the GBGR8ToGRGB8 converter (un)compression -
    // unsupported on the CPU load path for now.
    {DXGI_FORMAT_G8R8_G8B8_UNORM, DXGI_FORMAT_G8R8_G8B8_UNORM, DXGI_FORMAT_UNKNOWN, true,
     DXGI_FORMAT_R8G8B8A8_UNORM, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB, nullptr, 1, 8},
    // k_Y1_Cr_Y0_Cb_REP
    {DXGI_FORMAT_R8G8_B8G8_UNORM, DXGI_FORMAT_R8G8_B8G8_UNORM, DXGI_FORMAT_UNKNOWN, true,
     DXGI_FORMAT_R8G8B8A8_UNORM, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB, nullptr, 1, 8},
    // k_16_16_EDRAM
    // Not usable as a texture, also has -32...32 range.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG, nullptr, 1, 1},
    // k_8_8_8_8_A
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, nullptr, 1, 1},
    // k_4_4_4_4
    // Red and blue swapped in the converter for simplicity.
    {DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_UNKNOWN, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, ConvertRunRGBA4ToBGRA4, 1, 2},
    // k_10_11_11
    // Needs the R11G11B10ToRGBA16 expansion (and a separate signed version) -
    // unsupported on the CPU load path for now.
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     DXGI_FORMAT_R16G16B16A16_SNORM, false, DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB,
     nullptr, 1, 8},
    // k_11_11_10
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     DXGI_FORMAT_R16G16B16A16_SNORM, false, DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB,
     nullptr, 1, 8},
    // k_DXT1
    {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_UNKNOWN, true,
     DXGI_FORMAT_R8G8B8A8_UNORM, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, ConvertRunCopy, 1, 8},
    // k_DXT2_3
    {DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_UNKNOWN, true,
     DXGI_FORMAT_R8G8B8A8_UNORM, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, ConvertRunCopy, 1, 16},
    // k_DXT4_5
    {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_UNKNOWN, true,
     DXGI_FORMAT_R8G8B8A8_UNORM, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, ConvertRunCopy, 1, 16},
    // k_16_16_16_16_EDRAM
    // Not usable as a texture, also has -32...32 range.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, nullptr, 1, 1},
    // k_24_8
    // R32_FLOAT for depth because shaders would require an additional SRV to
    // sample stencil, which we don't provide (same as Direct3D 12).
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, ConvertRunDepth24UnormToR32, 1,
     4},
    // k_24_8_FLOAT
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, ConvertRunDepth24FloatToR32, 1,
     4},
    // k_16
    {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, ConvertRunCopy, 1, 2},
    // k_16_16
    {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG, ConvertRunCopy, 1, 4},
    // k_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     DXGI_FORMAT_R16G16B16A16_SNORM, false, DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA,
     ConvertRunCopy, 1, 8},
    // k_16_EXPAND
    {DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, ConvertRunCopy, 1, 2},
    // k_16_16_EXPAND
    {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG, ConvertRunCopy, 1, 4},
    // k_16_16_16_16_EXPAND
    {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
     false, DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, ConvertRunCopy, 1, 8},
    // k_16_FLOAT
    {DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, ConvertRunCopy, 1, 2},
    // k_16_16_FLOAT
    {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG, ConvertRunCopy, 1, 4},
    // k_16_16_16_16_FLOAT
    {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
     false, DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, ConvertRunCopy, 1, 8},
    // k_32
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, nullptr, 1, 1},
    // k_32_32
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG, nullptr, 1, 1},
    // k_32_32_32_32
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, nullptr, 1, 1},
    // k_32_FLOAT
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, ConvertRunCopy, 1, 4},
    // k_32_32_FLOAT
    {DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG, ConvertRunCopy, 1, 8},
    // k_32_32_32_32_FLOAT
    {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
     false, DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, ConvertRunCopy, 1, 16},
    // k_32_AS_8
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, nullptr, 1, 1},
    // k_32_AS_8_8
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG, nullptr, 1, 1},
    // k_16_MPEG
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, nullptr, 1, 1},
    // k_16_16_MPEG
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG, nullptr, 1, 1},
    // k_8_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, nullptr, 1, 1},
    // k_32_AS_8_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, nullptr, 1, 1},
    // k_32_AS_8_8_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG, nullptr, 1, 1},
    // k_16_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, nullptr, 1, 1},
    // k_16_MPEG_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, nullptr, 1, 1},
    // k_16_16_MPEG_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG, nullptr, 1, 1},
    // k_DXN
    {DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_UNKNOWN, true,
     DXGI_FORMAT_R8G8_UNORM, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG, ConvertRunCopy, 1, 16},
    // k_8_8_8_8_AS_16_16_16_16
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, ConvertRunCopy, 1, 4},
    // k_DXT1_AS_16_16_16_16
    {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_UNKNOWN, true,
     DXGI_FORMAT_R8G8B8A8_UNORM, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, ConvertRunCopy, 1, 8},
    // k_DXT2_3_AS_16_16_16_16
    {DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_UNKNOWN, true,
     DXGI_FORMAT_R8G8B8A8_UNORM, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, ConvertRunCopy, 1, 16},
    // k_DXT4_5_AS_16_16_16_16
    {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_UNKNOWN, true,
     DXGI_FORMAT_R8G8B8A8_UNORM, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, ConvertRunCopy, 1, 16},
    // k_2_10_10_10_AS_16_16_16_16
    {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_UNKNOWN, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, ConvertRunCopy, 1, 4},
    // k_10_11_11_AS_16_16_16_16
    // Needs the R11G11B10ToRGBA16 expansion (and a separate signed version) -
    // unsupported on the CPU load path for now.
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     DXGI_FORMAT_R16G16B16A16_SNORM, false, DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB,
     nullptr, 1, 8},
    // k_11_11_10_AS_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     DXGI_FORMAT_R16G16B16A16_SNORM, false, DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB,
     nullptr, 1, 8},
    // k_32_32_32_FLOAT
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB, nullptr, 1, 1},
    // k_DXT3A
    // R8_UNORM has the same size as BC2, but doesn't have the 4x4 size
    // alignment requirement.
    {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, ConvertRunDXT3AToR8, 4, 4},
    // k_DXT5A
    {DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_UNKNOWN, true,
     DXGI_FORMAT_R8_UNORM, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR, ConvertRunCopy, 1, 8},
    // k_CTX1
    {DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_UNKNOWN, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG, ConvertRunCTX1ToR8G8, 4, 8},
    // k_DXT3A_AS_1_1_1_1
    // Needs the DXT3AAs1111ToBGRA4 expansion - unsupported on the CPU load
    // path for now.
    {DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_UNKNOWN, false,
     DXGI_FORMAT_UNKNOWN, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, nullptr, 1, 2},
    // k_8_8_8_8_GAMMA_EDRAM
    // Not usable as a texture.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, nullptr, 1, 1},
    // k_2_10_10_10_FLOAT_EDRAM
    // Not usable as a texture.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, false, DXGI_FORMAT_UNKNOWN,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA, nullptr, 1, 1},
};

}  // namespace

// Creates a shader resource view of a host texture: the main unsigned (or
// float) view, or a variant - a signed normalized view, or a view of a stacked
// texture as a 2D array (for 2D fetches of 3D textures).
bool CreateHostSRV(ID3D11Device* device, ID3D11Resource* resource, xenos::DataDimension dimension,
                   uint32_t level_count, uint32_t depth_or_array_size, DXGI_FORMAT format,
                   bool as_2d_array, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv_out) {
  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  srv_desc.Format = format;
  if (dimension == xenos::DataDimension::k3D && as_2d_array) {
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srv_desc.Texture2DArray.MostDetailedMip = 0;
    srv_desc.Texture2DArray.MipLevels = level_count;
    srv_desc.Texture2DArray.FirstArraySlice = 0;
    srv_desc.Texture2DArray.ArraySize = depth_or_array_size;
  } else {
    switch (dimension) {
      case xenos::DataDimension::k3D:
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        srv_desc.Texture3D.MostDetailedMip = 0;
        srv_desc.Texture3D.MipLevels = level_count;
        srv_desc.Texture3D.MostDetailedMip = 0;
        break;
      case xenos::DataDimension::kCube:
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        srv_desc.TextureCube.MostDetailedMip = 0;
        srv_desc.TextureCube.MipLevels = level_count;
        break;
      default:
        if (depth_or_array_size > 1) {
          srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
          srv_desc.Texture2DArray.MostDetailedMip = 0;
          srv_desc.Texture2DArray.MipLevels = level_count;
          srv_desc.Texture2DArray.FirstArraySlice = 0;
          srv_desc.Texture2DArray.ArraySize = depth_or_array_size;
        } else {
          srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
          srv_desc.Texture2D.MostDetailedMip = 0;
          srv_desc.Texture2D.MipLevels = level_count;
        }
        break;
    }
  }
  return SUCCEEDED(device->CreateShaderResourceView(resource, &srv_desc, &srv_out));
}

D3D11TextureCache::D3D11Texture::D3D11Texture(D3D11TextureCache& texture_cache,
                                              const TextureKey& key, ID3D11Resource* resource,
                                              ID3D11ShaderResourceView* srv,
                                              uint64_t host_memory_usage, bool track_usage)
    : Texture(texture_cache, key, track_usage) {
  resource_ = resource;
  srv_ = srv;
  SetHostMemoryUsage(host_memory_usage);
}

D3D11TextureCache::D3D11Texture::~D3D11Texture() = default;

D3D11TextureCache::~D3D11TextureCache() {
  Shutdown(true);
}

bool D3D11TextureCache::Initialize() {
  REXGPU_INFO("D3D11: texture cache initialized (CPU untile/convert load path)");
  return true;
}

void D3D11TextureCache::Shutdown(bool from_destructor) {
  (void)from_destructor;
  if (texture_upload_count_) {
    REXGPU_INFO("D3D11 texture cache: uploaded {} textures ({} KB)", texture_upload_count_,
                texture_upload_bytes_ / 1024);
    texture_upload_count_ = 0;
    texture_upload_bytes_ = 0;
    texture_uploads_logged_ = 0;
  }
  load_scratch_base_.clear();
  load_scratch_mips_.clear();
  staging_.clear();
}

uint32_t D3D11TextureCache::GetHostFormatSwizzle(TextureKey key) const {
  return kHostFormats[uint32_t(key.format)].swizzle;
}

uint32_t D3D11TextureCache::GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const {
  switch (dimension) {
    case xenos::DataDimension::k1D:
    case xenos::DataDimension::k2DOrStacked:
      // D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION, valid for all 2D-based dimensions.
      return 16384;
    case xenos::DataDimension::k3D:
      // D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION.
      return 2048;
    case xenos::DataDimension::kCube:
      // D3D11_REQ_TEXTURECUBE_DIMENSION.
      return 16384;
    default:
      assert_unhandled_case(dimension);
      return 0;
  }
}

uint32_t D3D11TextureCache::GetMaxHostTextureDepthOrArraySize(
    xenos::DataDimension dimension) const {
  (void)dimension;
  // D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION / D3D11_REQ_TEXTURE3D_U_V_OR_W_DIMENSION.
  return 2048;
}

bool D3D11TextureCache::IsDecompressionNeeded(xenos::TextureFormat format, uint32_t width,
                                              uint32_t height) {
  DXGI_FORMAT dxgi_format_uncompressed = kHostFormats[uint32_t(format)].dxgi_format_uncompressed;
  if (dxgi_format_uncompressed == DXGI_FORMAT_UNKNOWN) {
    return false;
  }
  const FormatInfo* format_info = FormatInfo::Get(format);
  return (width & (format_info->block_width - 1)) || (height & (format_info->block_height - 1));
}

void D3D11TextureCache::LogUnsupportedFormatOnce(TextureKey key) {
  uint64_t format_bit = UINT64_C(1) << uint32_t(key.format);
  if (unsupported_format_mask_logged_ & format_bit) {
    return;
  }
  unsupported_format_mask_logged_ |= format_bit;
  key.LogAction("rejected by the D3D11 CPU texture load path (format conversion unsupported)");
}

std::unique_ptr<TextureCache::Texture> D3D11TextureCache::CreateTexture(TextureKey key) {
  SCOPE_profile_cpu_f("gpu");
  const HostFormat& host_format = kHostFormats[uint32_t(key.format)];
  if (host_format.dxgi_format_resource == DXGI_FORMAT_UNKNOWN || !host_format.convert) {
    LogUnsupportedFormatOnce(key);
    return nullptr;
  }
  if (IsDecompressionNeeded(key.format, key.GetWidth(), key.GetHeight())) {
    // The decompressing conversions (DXT to RGBA8 and the 4:2:2 unchroma) are
    // not on the CPU load path yet.
    LogUnsupportedFormatOnce(key);
    return nullptr;
  }
  if (key.scaled_resolve) {
    // Resolution-scaled resolves live in scaled resolve memory - Phase 4
    // work. The base defers these loads while the pool is uncommitted anyway.
    return nullptr;
  }

  ID3D11Device* device = command_processor_.GetD3D11Provider().GetDevice();
  if (!device) {
    return nullptr;
  }

  const uint32_t level_count = key.mip_max_level + 1;
  Microsoft::WRL::ComPtr<ID3D11Resource> resource;
  if (key.dimension == xenos::DataDimension::k3D) {
    D3D11_TEXTURE3D_DESC desc = {};
    desc.Width = key.GetWidth();
    desc.Height = key.GetHeight();
    desc.Depth = key.GetDepthOrArraySize();
    desc.MipLevels = level_count;
    desc.Format = host_format.dxgi_format_resource;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Microsoft::WRL::ComPtr<ID3D11Texture3D> texture_3d;
    if (FAILED(device->CreateTexture3D(&desc, nullptr, &texture_3d))) {
      key.LogAction("CreateTexture3D failed");
      return nullptr;
    }
    resource = texture_3d;
  } else {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = key.GetWidth();
    desc.Height = key.GetHeight();
    desc.MipLevels = level_count;
    desc.ArraySize = key.GetDepthOrArraySize();
    desc.Format = host_format.dxgi_format_resource;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (key.dimension == xenos::DataDimension::kCube) {
      desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_2d;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture_2d))) {
      key.LogAction("CreateTexture2D failed");
      return nullptr;
    }
    resource = texture_2d;
  }

  // The unsigned (or float) shader resource view - the Phase 3 binding path
  // uses it directly; variant views (signed, or stacked viewed as 2D) are
  // created there on demand.
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
  if (!CreateHostSRV(device, resource.Get(), key.dimension, level_count,
                     key.GetDepthOrArraySize(), host_format.dxgi_format_unsigned, false, srv)) {
    key.LogAction("CreateShaderResourceView failed");
    return nullptr;
  }

  // Estimated host memory usage for LRU accounting - the guest extents (the
  // host image has the same block layout for the supported formats).
  const texture_util::TextureGuestLayout guest_layout = key.GetGuestLayout();
  return std::make_unique<D3D11Texture>(
      *this, key, resource.Get(), srv.Get(),
      uint64_t(guest_layout.base.level_data_extent_bytes) + guest_layout.mips_total_extent_bytes);
}

bool D3D11TextureCache::LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                                              bool load_mips) {
  SCOPE_profile_cpu_f("gpu");
  D3D11Texture& d3d11_texture = static_cast<D3D11Texture&>(texture);
  const TextureKey key = d3d11_texture.key();
  const HostFormat& host_format = kHostFormats[uint32_t(key.format)];
  if (!host_format.convert) {
    return false;
  }
  if (key.scaled_resolve) {
    // Scaled resolve memory is Phase 4 work; the base defers these while the
    // scaled resolve pool is uncommitted anyway.
    return false;
  }

  const texture_util::TextureGuestLayout& guest_layout = d3d11_texture.guest_layout();
  const xenos::DataDimension dimension = key.dimension;
  const bool is_3d = dimension == xenos::DataDimension::k3D;
  const bool is_3d_tiling = is_3d || d3d11_texture.force_load_3d_tiling();
  const uint32_t width = key.GetWidth();
  const uint32_t height = key.GetHeight();
  const uint32_t depth_or_array_size = key.GetDepthOrArraySize();
  const uint32_t depth = is_3d ? depth_or_array_size : 1;
  const uint32_t array_size = is_3d ? 1 : depth_or_array_size;
  const xenos::TextureFormat guest_format = key.format;
  const FormatInfo* guest_format_info = FormatInfo::Get(guest_format);
  const uint32_t block_width = guest_format_info->block_width;
  const uint32_t block_height = guest_format_info->block_height;
  const uint32_t bytes_per_block = guest_format_info->bytes_per_block();
  const uint32_t level_count = key.mip_max_level + 1;
  const uint32_t level_first = load_base ? 0 : 1;
  const uint32_t level_last = load_mips ? key.mip_max_level : 0;
  if (level_first > level_last) {
    return true;
  }
  const uint32_t level_packed = guest_layout.packed_level;

  ID3D11DeviceContext* context = command_processor_.GetD3D11Provider().GetImmediateContext();
  if (!context) {
    return false;
  }

  // Make the guest spans CPU-authoritative (the same spans
  // CommitPreparedTextureLoad made resident) - rejects the load if any page
  // was GPU-written (a resolve source), which the base defers to be reloaded
  // from scaled resolve memory later.
  // fh1_swap_fetch_trace: a load rejected here leaves the host texture at
  // whatever it held (or empty on first use) while the guest thinks it just
  // sampled the range - the "draw happened, nothing appeared" symptom. The
  // base and the page states name which range was refused and why.
  auto log_rejected_load = [&](const char* span_name, uint32_t start, uint32_t size) {
    if (!REXCVAR_GET(fh1_swap_fetch_trace)) {
      return;
    }
    static uint32_t rejected_logs = 0;
    if (rejected_logs >= 60) {
      return;
    }
    ++rejected_logs;
    const auto pages = shared_memory().CountValidPages(start, size);
    REXGPU_INFO(
        "FH1 texture load rejected: {} base={:08X}+{} {}x{}x{} format={} tiled={} pages "
        "valid={} gpu_written={}",
        span_name, start, size, key.GetWidth(), key.GetHeight(), key.GetDepthOrArraySize(),
        uint32_t(key.format), uint32_t(key.tiled), pages.first, pages.second);
  };
  // A resolve writes the GPU mirror of shared memory, not guest memory. Before
  // a CPU-side untiling read of a range the GPU wrote (this is the only place a
  // resolved texture's pixels can come from in this backend), land the mirror's
  // bytes for it in guest memory. Without this the range's pages stay
  // GPU-written, CopyCpuRange below refuses them, and every texture the guest
  // resolved and then samples - the 3D scene, most visibly - would never be
  // uploaded.
  if (REXCVAR_GET(fh1_resolve_guest_refresh)) {
    if (load_base) {
      command_processor_.RefreshResolvedGuestRange(uint32_t(key.base_page) << 12,
                                                   d3d11_texture.GetGuestBaseSize());
    }
    if (load_mips) {
      command_processor_.RefreshResolvedGuestRange(uint32_t(key.mip_page) << 12,
                                                   d3d11_texture.GetGuestMipsSize());
    }
  }
  if (load_base) {
    const uint32_t base_addr = uint32_t(key.base_page) << 12;
    const uint32_t base_size = d3d11_texture.GetGuestBaseSize();
    load_scratch_base_.resize(base_size);
    if (!shared_memory().CopyCpuRange(base_addr,
                                      std::span<uint8_t>(load_scratch_base_.data(), base_size))) {
      log_rejected_load("base", base_addr, base_size);
      return false;
    }
  }
  if (load_mips) {
    const uint32_t mips_addr = uint32_t(key.mip_page) << 12;
    const uint32_t mips_size = d3d11_texture.GetGuestMipsSize();
    load_scratch_mips_.resize(mips_size);
    if (!shared_memory().CopyCpuRange(mips_addr,
                                      std::span<uint8_t>(load_scratch_mips_.data(), mips_size))) {
      log_rejected_load("mips", mips_addr, mips_size);
      return false;
    }
  }

  // Level addressing mirrors the Direct3D 12 load shader dispatch: the base
  // level is under base_page, stored mips min(1, packed_level)...min(mip_max,
  // packed_level) are under mip_page at mip_offsets_bytes offsets, and levels
  // at or above the packed level are windows at GetPackedMipOffset origins
  // within their storage.
  uint64_t uploaded_bytes = 0;
  for (uint32_t level = level_first; level <= level_last; ++level) {
    const bool is_base = level == 0;
    const uint32_t stored_level = is_base ? 0 : std::min(level, level_packed);
    const texture_util::TextureGuestLayout::Level& level_layout =
        is_base ? guest_layout.base : guest_layout.mips[stored_level];
    const uint8_t* source = (is_base ? load_scratch_base_ : load_scratch_mips_).data();
    if (!is_base) {
      source += guest_layout.mip_offsets_bytes[stored_level];
    }
    uint32_t offset_x_blocks = 0;
    uint32_t offset_y_blocks = 0;
    uint32_t offset_z = 0;
    if (level >= level_packed) {
      texture_util::GetPackedMipOffset(width, height, depth, guest_format, level, offset_x_blocks,
                                       offset_y_blocks, offset_z);
    }
    const uint32_t level_width = std::max(width >> level, uint32_t(1));
    const uint32_t level_height = std::max(height >> level, uint32_t(1));
    const uint32_t level_depth = std::max(depth >> level, uint32_t(1));
    const uint32_t width_blocks = (level_width + block_width - 1) / block_width;
    const uint32_t height_blocks = (level_height + block_height - 1) / block_height;
    const uint32_t pitch_blocks = level_layout.row_pitch_bytes / bytes_per_block;
    if (key.tiled) {
      assert_zero(pitch_blocks & (xenos::kTextureTileWidthHeight - 1));
    }

    // Staging image of one level slice (all Z slices for 3D levels).
    const uint32_t row_bytes = width_blocks * host_format.convert_block_row_bytes;
    const uint32_t row_pitch_bytes = rex::align(row_bytes, UINT32_C(256));
    const uint32_t rows_per_slice = height_blocks * host_format.convert_rows;
    const uint32_t slice_pitch_bytes = row_pitch_bytes * rows_per_slice;
    staging_.resize(size_t(slice_pitch_bytes) * level_depth);

    for (uint32_t slice = 0; slice < array_size; ++slice) {
      texture_conversion::GuestLevelSlice slice_info = {};
      slice_info.source = source + size_t(slice) * level_layout.array_slice_stride_bytes;
      slice_info.tiled = key.tiled != 0;
      slice_info.tiled_3d = is_3d_tiling;
      slice_info.pitch_blocks = pitch_blocks;
      slice_info.height_blocks_storage = level_layout.z_slice_stride_block_rows;
      slice_info.offset_x_blocks = offset_x_blocks;
      slice_info.offset_y_blocks = offset_y_blocks;
      slice_info.offset_z = offset_z;
      slice_info.width_blocks = width_blocks;
      slice_info.height_blocks = height_blocks;
      slice_info.depth = level_depth;
      slice_info.z_slice_stride_block_rows = level_layout.z_slice_stride_block_rows;
      slice_info.source_block_bytes = bytes_per_block;
      slice_info.destination = staging_.data();
      slice_info.destination_pitch_bytes = row_pitch_bytes;
      slice_info.destination_rows_per_block_row = host_format.convert_rows;
      slice_info.destination_block_row_bytes = host_format.convert_block_row_bytes;
      slice_info.endian = key.endianness;
      slice_info.convert = host_format.convert;
      texture_conversion::CopyGuestLevelSlice(slice_info);

      // Phase 3 M3: while a draw's RequestTextures is running, the upload is a
      // packet on the command tape - the CPU untiling above still happens here,
      // the GPU write happens at replay, before the draw that samples it. The
      // swap's upload (and any load outside a draw) stays immediate.
      const uint32_t destination_subresource = level + slice * level_count;
      if (!command_processor_.RecordTextureUpload(
              d3d11_texture.resource(), destination_subresource, staging_.data(),
              slice_pitch_bytes, row_pitch_bytes, slice_pitch_bytes)) {
        context->UpdateSubresource(d3d11_texture.resource(), destination_subresource, nullptr,
                                   staging_.data(), row_pitch_bytes, slice_pitch_bytes);
      }
      uploaded_bytes += slice_pitch_bytes;
    }
  }

  // Update LRU caching because the texture will be used.
  d3d11_texture.MarkAsUsed();

  texture_upload_count_ += 1;
  texture_upload_bytes_ += uploaded_bytes;
  if (texture_upload_count_ <= 1) {
    REXGPU_INFO(
        "D3D11 texture cache: uploaded the first texture ({}x{}x{}, format {}, {} level(s), "
        "{} KB)",
        width, height, depth_or_array_size, uint32_t(key.format), level_count,
        uploaded_bytes / 1024);
  } else if (texture_upload_count_ >= uint64_t(texture_uploads_logged_) + 500) {
    texture_uploads_logged_ = uint32_t(texture_upload_count_);
    REXGPU_INFO("D3D11 texture cache: uploaded {} textures ({} KB)", texture_upload_count_,
                texture_upload_bytes_ / 1024);
  }
  return true;
}

bool D3D11TextureCache::GetSwapGuestExtent(uint32_t& base_out, uint32_t& length_out) {
  const xenos::xe_gpu_texture_fetch_t fetch = register_file().GetTextureFetch(0);
  TextureKey key;
  BindingInfoFromFetchConstant(fetch, key, nullptr);
  if (!key.is_valid || key.base_page == 0 ||
      key.dimension != xenos::DataDimension::k2DOrStacked) {
    return false;
  }
  // FindOrCreateTexture only lays the host texture out to the guest key - the
  // pixels are uploaded by LoadTextureData, which the readback must precede -
  // and GetGuestBaseSize is the base level's byte size in guest memory, the
  // extent the swap's untiling upload reads.
  auto* texture = static_cast<D3D11Texture*>(FindOrCreateTexture(key));
  if (!texture) {
    return false;
  }
  base_out = uint32_t(key.base_page) << 12;
  length_out = uint32_t(texture->GetGuestBaseSize());
  return true;
}

ID3D11ShaderResourceView* D3D11TextureCache::RequestSwapTexture(xenos::TextureFormat& format_out,
                                                               uint32_t& width_out,
                                                               uint32_t& height_out) {
  // Port of the Direct3D 12 RequestSwapTexture: the swap packet's front buffer
  // description is texture fetch constant 0, and the pixels are the ones the
  // resolve path wrote into guest memory (IssueCopy), so the texture is loaded
  // through the normal guest-memory upload path.
  const xenos::xe_gpu_texture_fetch_t fetch = register_file().GetTextureFetch(0);
  TextureKey key;
  BindingInfoFromFetchConstant(fetch, key, nullptr);
  if (!key.is_valid || key.base_page == 0 ||
      key.dimension != xenos::DataDimension::k2DOrStacked) {
    if (!swap_request_logged_) {
      swap_request_logged_ = true;
      REXGPU_WARN(
          "D3D11 texture cache: the swap fetch is not a 2D texture ({}:{}:{:08X} {:08X} {:08X} "
          "{:08X} {:08X} {:08X})",
          uint32_t(key.is_valid), uint32_t(key.base_page), fetch.dword_0, fetch.dword_1,
          fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5);
    }
    return nullptr;
  }
  auto* texture = static_cast<D3D11Texture*>(FindOrCreateTexture(key));
  if (!texture) {
    if (swap_fail_logs_ < 3) {
      ++swap_fail_logs_;
      REXGPU_WARN("D3D11 texture cache: the swap texture could not be created ({}x{}x{}, "
                  "format {})",
                  key.GetWidth(), key.GetHeight(), key.GetDepthOrArraySize(),
                  uint32_t(key.format));
    }
    return nullptr;
  }
  // The resolve wrote the frontbuffer straight into the host-visible guest
  // memory mirror, so after the swap's GPU wait the CPU owns those bytes: mark
  // the texture's whole span as CPU-valid (this also invalidates the cached
  // texture, so the load below uploads the current frame). Without it the
  // pages stay flagged as GPU-written, CopyCpuRange refuses them, and the
  // cached texture is frozen at its first upload.
  shared_memory().MemoryInvalidationCallback(uint32_t(key.base_page) << 12,
                                             uint32_t(texture->GetGuestBaseSize()), true);
  const bool swap_upload_ok = LoadTextureData(*texture);
  if (REXCVAR_GET(fh1_swap_fetch_trace)) {
    static uint32_t swap_upload_logs = 0;
    if (!(++swap_upload_logs % 120) && !load_scratch_base_.empty()) {
      uint64_t nonzero = 0;
      for (size_t i = 0; i < load_scratch_base_.size() && i < (1u << 20); i += 4) {
        nonzero += load_scratch_base_[i] != 0;
      }
      const uint32_t key_base = uint32_t(texture->key().base_page) << 12;
      uint32_t guest_first = 0;
      std::span<uint8_t> guest_word(reinterpret_cast<uint8_t*>(&guest_first), sizeof(guest_first));
      const bool guest_read = shared_memory().CopyCpuSnapshot(key_base, guest_word);
      REXGPU_INFO(
          "FH1 swap upload source: key_base={:08X} {} bytes read, "
          "nonzero[sampled 1 in 4 of the first 1 MB]={}, guest_first={:08X} (read={}) "
          "scratch_first={:08X}",
          key_base, load_scratch_base_.size(), nonzero, guest_first, guest_read,
          load_scratch_base_.size() >= 4
              ? reinterpret_cast<const uint32_t*>(load_scratch_base_.data())[0]
              : 0);
    }
  }
  if (!swap_upload_ok || !texture->srv()) {
    if (swap_fail_logs_ < 3) {
      ++swap_fail_logs_;
      REXGPU_WARN("D3D11 texture cache: the swap texture could not be uploaded ({}x{}x{}, "
                  "format {})",
                  key.GetWidth(), key.GetHeight(), key.GetDepthOrArraySize(),
                  uint32_t(key.format));
    }
    return nullptr;
  }
  texture->MarkAsUsed();
  // fh1_debug_presented_pair: the upload just landed, so this is the presented
  // leg as the presenter is about to paint it (borrowed pointer, cache-owned).
  swap_resource_ = texture->resource();
  // Only the texture, not the fetch constant, knows whether it is scaled.
  key = texture->key();
  format_out = key.format;
  width_out = key.GetWidth();
  height_out = key.GetHeight();
  // The caller holds the view until the swap has drawn with it - the cache may
  // otherwise evict the texture (the view keeps the resource alive).
  texture->srv()->AddRef();
  return texture->srv();
}

ID3D11Texture2D* D3D11TextureCache::FindTexture2DForGuestBase(uint32_t guest_base,
                                                              xenos::TextureFormat guest_format,
                                                              DXGI_FORMAT& read_format_out,
                                                              uint32_t& width_out,
                                                              uint32_t& height_out) {
  // Normalized the way BindingInfoFromFetchConstant normalizes it (a
  // k_8_8_8_8_AS_16_16_16_16 fetch is stored as k_8_8_8_8), so the texture
  // this finds is the one the draw binds, not a sibling that merely shares
  // the base.
  const xenos::TextureFormat base_format = rex::graphics::GetBaseFormat(guest_format);
  for (Texture* texture : TexturesAtBase(guest_base)) {
    if (texture->key().format != base_format) {
      continue;
    }
    auto* d3d11_texture = static_cast<D3D11Texture*>(texture);
    ID3D11ShaderResourceView* srv = d3d11_texture->srv();
    if (!srv) {
      continue;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    srv->GetDesc(&srv_desc);
    if (srv_desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D &&
        srv_desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2DARRAY) {
      continue;
    }
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    srv->GetResource(&resource);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_2d;
    if (!resource || FAILED(resource.As(&texture_2d)) || !texture_2d) {
      continue;
    }
    D3D11_TEXTURE2D_DESC desc;
    texture_2d->GetDesc(&desc);
    // The SRV's own typed format: the resource may be typeless, and this is
    // what the shader samples - so the readback shows the sampled texels.
    read_format_out = srv_desc.Format;
    width_out = desc.Width;
    height_out = desc.Height;
    // Borrowed - the cache's D3D11Texture owns the resource.
    return texture_2d.Get();
  }
  return nullptr;
}

ID3D11Texture2D* D3D11TextureCache::FindAnyTexture2DForGuestBase(uint32_t guest_base,
                                                                DXGI_FORMAT& read_format_out,
                                                                uint32_t& width_out,
                                                                uint32_t& height_out,
                                                                uint32_t& host_format_out) {
  for (Texture* texture : TexturesAtBase(guest_base)) {
    auto* d3d11_texture = static_cast<D3D11Texture*>(texture);
    ID3D11ShaderResourceView* srv = d3d11_texture->srv();
    if (!srv) {
      continue;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    srv->GetDesc(&srv_desc);
    if (srv_desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D &&
        srv_desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2DARRAY) {
      continue;
    }
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    srv->GetResource(&resource);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_2d;
    if (!resource || FAILED(resource.As(&texture_2d)) || !texture_2d) {
      continue;
    }
    D3D11_TEXTURE2D_DESC desc;
    texture_2d->GetDesc(&desc);
    read_format_out = srv_desc.Format;
    width_out = desc.Width;
    height_out = desc.Height;
    host_format_out = uint32_t(texture->key().format);
    return texture_2d.Get();
  }
  return nullptr;
}

ID3D11ShaderResourceView* D3D11TextureCache::GetActiveTextureBindfulSRV(
    const DxbcShader::TextureBinding& host_shader_binding) {
  // Port of the Direct3D 12 WriteActiveTextureBindfulSRV: unsigned vs signed
  // by the fetch's sign bits, with null (zero-reading) slots for invalid or
  // incompatible fetches.
  uint32_t fetch_constant_index = host_shader_binding.fetch_constant;
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  // fh1_debug_texture_dump (PHASE 10): the binding path's own verdict for a
  // frame-sized fetch. A dump taken before the draw sees only what the cache
  // already holds; this is the key the draw actually binds, or the null SRV it
  // falls back to - which is the difference between sampling the frame and
  // sampling zero. Deduplicated on the fetch's format and the bound key, so a
  // per-frame pass logs once per distinct outcome.
  {
    const xenos::xe_gpu_texture_fetch_t diag_fetch =
        register_file().GetTextureFetch(fetch_constant_index);
    if (!REXCVAR_GET(fh1_debug_texture_dump).empty() &&
        uint32_t(diag_fetch.size_2d.width) + 1 >= 1280 &&
        uint32_t(diag_fetch.size_2d.height) + 1 >= 720) {
      static uint32_t diag_logs = 0;
      static uint64_t diag_last = UINT64_MAX;
      const uint64_t diag_state =
          (uint64_t(uint32_t(diag_fetch.format)) << 32) |
          (binding ? uint64_t(uint32_t(binding->key.format)) << 8 |
                         uint64_t(binding->texture ? 1 : 0) |
                         uint64_t(binding->texture &&
                                          static_cast<D3D11Texture*>(binding->texture)->srv()
                                      ? 2
                                      : 0)
                   : 0xFFu);
      if (diag_state != diag_last && diag_logs < 40) {
        diag_last = diag_state;
        ++diag_logs;
        if (binding) {
          REXGPU_INFO(
              "FH1 srv bind: swap={} slot={} base={:08X} fetch={}x{} fmt={} tiled={} -> key fmt={} "
              "{}x{} texture={} srv={}",
              command_processor_.presented_output_frames(), fetch_constant_index,
              uint32_t(diag_fetch.base_address) << 12,
              uint32_t(diag_fetch.size_2d.width) + 1, uint32_t(diag_fetch.size_2d.height) + 1,
              uint32_t(diag_fetch.format), uint32_t(diag_fetch.tiled),
              uint32_t(binding->key.format), binding->key.GetWidth(), binding->key.GetHeight(),
              binding->texture ? 1 : 0,
              binding->texture && static_cast<D3D11Texture*>(binding->texture)->srv() ? 1 : 0);
        } else {
          REXGPU_INFO(
              "FH1 srv bind: swap={} slot={} base={:08X} fetch={}x{} fmt={} tiled={} -> no valid "
              "binding (the fetch cannot be hosted)",
              command_processor_.presented_output_frames(), fetch_constant_index,
              uint32_t(diag_fetch.base_address) << 12,
              uint32_t(diag_fetch.size_2d.width) + 1, uint32_t(diag_fetch.size_2d.height) + 1,
              uint32_t(diag_fetch.format), uint32_t(diag_fetch.tiled));
        }
      }
    }
  }
  if (!binding || !AreDimensionsCompatible(host_shader_binding.dimension, binding->key.dimension)) {
    return nullptr;
  }
  bool force_special_view = binding->key.dimension == xenos::DataDimension::k3D &&
                            (host_shader_binding.dimension == xenos::FetchOpDimension::k1D ||
                             host_shader_binding.dimension == xenos::FetchOpDimension::k2D);
  Texture* texture = nullptr;
  bool use_signed = false;
  if (host_shader_binding.is_signed) {
    if (texture_util::IsAnySignSigned(binding->swizzled_signs)) {
      texture = IsSignedVersionSeparateForFormat(binding->key) ? binding->texture_signed
                                                               : binding->texture;
      use_signed = true;
    }
  } else {
    if (texture_util::IsAnySignNotSigned(binding->swizzled_signs)) {
      texture = binding->texture;
    }
  }
  if (!texture) {
    return nullptr;
  }
  texture->MarkAsUsed();
  auto& d3d11_texture = *static_cast<D3D11Texture*>(texture);
  if (!use_signed && !force_special_view) {
    return d3d11_texture.srv();
  }
  uint32_t variant_key = uint32_t(use_signed) | uint32_t(force_special_view) << 1;
  if (d3d11_texture.variant_key() == variant_key && d3d11_texture.variant_srv()) {
    return d3d11_texture.variant_srv();
  }
  const HostFormat& host_format = kHostFormats[uint32_t(binding->key.format)];
  DXGI_FORMAT format = use_signed ? host_format.dxgi_format_signed : host_format.dxgi_format_unsigned;
  if (format == DXGI_FORMAT_UNKNOWN) {
    // No signed representation (block-compressed, for instance) - the
    // unsigned view is the closest sample.
    return d3d11_texture.srv();
  }
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> variant_srv;
  if (!CreateHostSRV(command_processor_.GetD3D11Provider().GetDevice(),
                     d3d11_texture.resource(), binding->key.dimension,
                     binding->key.mip_max_level + 1, binding->key.GetDepthOrArraySize(), format,
                     force_special_view, variant_srv)) {
    binding->key.LogAction("CreateShaderResourceView (variant) failed");
    return nullptr;
  }
  d3d11_texture.set_variant(variant_key, variant_srv);
  return d3d11_texture.variant_srv();
}

xenos::ClampMode D3D11TextureCache::NormalizeClampMode(xenos::ClampMode clamp_mode) const {
  if (clamp_mode == xenos::ClampMode::kClampToHalfway) {
    // No GL_CLAMP (clamp to half edge, half border) equivalent in Direct3D 11.
    return xenos::ClampMode::kClampToEdge;
  }
  if (clamp_mode == xenos::ClampMode::kMirrorClampToHalfway ||
      clamp_mode == xenos::ClampMode::kMirrorClampToBorder) {
    // No Direct3D 11 equivalents.
    return xenos::ClampMode::kMirrorClampToEdge;
  }
  return clamp_mode;
}

D3D11_SAMPLER_DESC D3D11TextureCache::GetSamplerDesc(
    const DxbcShader::SamplerBinding& binding) const {
  // Port of the Direct3D 12 GetSamplerParameters + WriteSampler.
  const auto& regs = register_file();
  xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);

  SamplerParameters parameters;
  xenos::ClampMode fetch_clamp_x, fetch_clamp_y, fetch_clamp_z;
  texture_util::GetClampModesForDimension(fetch, fetch_clamp_x, fetch_clamp_y, fetch_clamp_z);
  parameters.clamp_x = NormalizeClampMode(fetch_clamp_x);
  parameters.clamp_y = NormalizeClampMode(fetch_clamp_y);
  parameters.clamp_z = NormalizeClampMode(fetch_clamp_z);
  if (xenos::ClampModeUsesBorder(parameters.clamp_x) ||
      xenos::ClampModeUsesBorder(parameters.clamp_y) ||
      xenos::ClampModeUsesBorder(parameters.clamp_z)) {
    parameters.border_color = fetch.border_color;
  } else {
    parameters.border_color = xenos::BorderColor::k_ABGR_Black;
  }

  uint32_t mip_min_level, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, nullptr, nullptr,
                                                 &mip_min_level, &mip_max_level);
  parameters.mip_min_level = mip_min_level;
  bool has_mips = mip_max_level > mip_min_level;

  xenos::TextureFilter mag_filter = binding.mag_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mag_filter
                                        : binding.mag_filter;
  xenos::TextureFilter min_filter = binding.min_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.min_filter
                                        : binding.min_filter;
  xenos::TextureFilter mip_filter = binding.mip_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mip_filter
                                        : binding.mip_filter;
  bool min_mag_linear = (mag_filter == xenos::TextureFilter::kLinear) &&
                        (min_filter == xenos::TextureFilter::kLinear);
  bool mip_filter_bilinear_or_trilinear =
      mip_filter == xenos::TextureFilter::kPoint || mip_filter == xenos::TextureFilter::kLinear;
  bool mip_base_map = mip_filter == xenos::TextureFilter::kBaseMap;
  if (REXCVAR_GET(force_trilinear_filtering) && has_mips && min_mag_linear &&
      mip_filter == xenos::TextureFilter::kPoint) {
    mip_filter = xenos::TextureFilter::kLinear;
  }

  xenos::AnisoFilter aniso_filter = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst
                                        ? fetch.aniso_filter
                                        : binding.aniso_filter;
  int32_t anisotropic_override = REXCVAR_GET(anisotropic_override);
  if (anisotropic_override > -1 && anisotropic_override < 6 && has_mips && !mip_base_map &&
      min_mag_linear && mip_filter_bilinear_or_trilinear) {
    aniso_filter = xenos::AnisoFilter(anisotropic_override);
  }
  aniso_filter = std::min(aniso_filter, xenos::AnisoFilter::kMax_16_1);
  parameters.aniso_filter = aniso_filter;
  if (aniso_filter != xenos::AnisoFilter::kDisabled) {
    parameters.mag_linear = 1;
    parameters.min_linear = 1;
    parameters.mip_linear = 1;
  } else {
    parameters.mag_linear = mag_filter == xenos::TextureFilter::kLinear;
    parameters.min_linear = min_filter == xenos::TextureFilter::kLinear;
    parameters.mip_linear = mip_filter == xenos::TextureFilter::kLinear;
  }
  parameters.mip_base_map = mip_base_map;

  D3D11_SAMPLER_DESC desc = {};
  if (parameters.aniso_filter != xenos::AnisoFilter::kDisabled) {
    desc.Filter = D3D11_FILTER_ANISOTROPIC;
    desc.MaxAnisotropy = 1u << (uint32_t(parameters.aniso_filter) - 1);
  } else {
    D3D11_FILTER_TYPE filter_min =
        parameters.min_linear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
    D3D11_FILTER_TYPE filter_mag =
        parameters.mag_linear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
    D3D11_FILTER_TYPE filter_mip =
        parameters.mip_linear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
    desc.Filter = D3D11_ENCODE_BASIC_FILTER(filter_min, filter_mag, filter_mip, 0);
    desc.MaxAnisotropy = 1;
  }
  static const D3D11_TEXTURE_ADDRESS_MODE kAddressModeMap[] = {
      /* kRepeat               */ D3D11_TEXTURE_ADDRESS_WRAP,
      /* kMirroredRepeat       */ D3D11_TEXTURE_ADDRESS_MIRROR,
      /* kClampToEdge          */ D3D11_TEXTURE_ADDRESS_CLAMP,
      /* kMirrorClampToEdge    */ D3D11_TEXTURE_ADDRESS_MIRROR_ONCE,
      /* kClampToHalfway       */ D3D11_TEXTURE_ADDRESS_CLAMP,
      /* kMirrorClampToHalfway */ D3D11_TEXTURE_ADDRESS_MIRROR_ONCE,
      /* kClampToBorder        */ D3D11_TEXTURE_ADDRESS_BORDER,
      /* kMirrorClampToBorder  */ D3D11_TEXTURE_ADDRESS_MIRROR_ONCE,
  };
  desc.AddressU = kAddressModeMap[uint32_t(parameters.clamp_x) & 7];
  desc.AddressV = kAddressModeMap[uint32_t(parameters.clamp_y) & 7];
  desc.AddressW = kAddressModeMap[uint32_t(parameters.clamp_z) & 7];
  // The title's LOD bias is applied in shaders; this is the player's global
  // offset on top of it.
  desc.MipLODBias = float(REXCVAR_GET(texture_mip_lod_bias));
  desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  switch (parameters.border_color) {
    case xenos::BorderColor::k_ABGR_White:
      desc.BorderColor[0] = 1.0f;
      desc.BorderColor[1] = 1.0f;
      desc.BorderColor[2] = 1.0f;
      desc.BorderColor[3] = 1.0f;
      break;
    case xenos::BorderColor::k_ACBYCR_Black:
      desc.BorderColor[0] = 0.5f;
      desc.BorderColor[1] = 0.0f;
      desc.BorderColor[2] = 0.5f;
      desc.BorderColor[3] = 0.0f;
      break;
    case xenos::BorderColor::k_ACBCRY_Black:
      desc.BorderColor[0] = 0.0f;
      desc.BorderColor[1] = 0.5f;
      desc.BorderColor[2] = 0.5f;
      desc.BorderColor[3] = 0.0f;
      break;
    default:
      desc.BorderColor[0] = 0.0f;
      desc.BorderColor[1] = 0.0f;
      desc.BorderColor[2] = 0.0f;
      desc.BorderColor[3] = 0.0f;
      break;
  }
  desc.MinLOD = float(parameters.mip_min_level);
  if (parameters.mip_base_map) {
    // "It is undefined whether LOD clamping based on MinLOD and MaxLOD Sampler
    // states should happen before or after deciding if magnification is
    // occuring" - Direct3D 11.3 Functional Specification. Same approach as the
    // Direct3D 12 cache: keep magnification vs. minification (MaxLOD 0.25),
    // except with anisotropic filtering where the filter is always linear.
    desc.MaxLOD = desc.MinLOD;
    if (parameters.aniso_filter == xenos::AnisoFilter::kDisabled) {
      desc.MaxLOD += 0.25f;
    }
  } else {
    // Maximum mip level is in the texture resource itself.
    desc.MaxLOD = FLT_MAX;
  }
  return desc;
}

}  // namespace rex::graphics::d3d11
