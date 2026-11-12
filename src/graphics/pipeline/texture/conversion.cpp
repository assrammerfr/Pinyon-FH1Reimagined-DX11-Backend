/**
 *******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 *******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 *******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

#include <rex/dbg.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>

namespace rex::graphics::texture_conversion {

using namespace rex::graphics::xenos;

void CopySwapBlock(xenos::Endian endian, void* output, const void* input, size_t length) {
  switch (endian) {
    case xenos::Endian::k8in16:
      memory::copy_and_swap_16_unaligned(output, input, length / 2);
      break;
    case xenos::Endian::k8in32:
      memory::copy_and_swap_32_unaligned(output, input, length / 4);
      break;
    case xenos::Endian::k16in32:  // Swap high and low 16 bits within a 32 bit
                                  // word
      memory::copy_and_swap_16_in_32_unaligned(output, input, length);
      break;
    default:
    case xenos::Endian::kNone:
      std::memcpy(output, input, length);
      break;
  }
}

void ConvertTexelCTX1ToR8G8(xenos::Endian endian, void* output, const void* input, size_t length) {
  // https://fileadmin.cs.lth.se/cs/Personal/Michael_Doggett/talks/unc-xenos-doggett.pdf
  // (R is in the higher bits, according to how this format is used in
  //  4D5307E6).
  union {
    uint8_t data[8];
    struct {
      uint8_t g0, r0, g1, r1;
      uint32_t xx;
    };
  } block;
  static_assert(sizeof(block) == 8, "CTX1 block mismatch");

  const uint32_t bytes_per_block = 8;
  CopySwapBlock(endian, block.data, input, bytes_per_block);

  uint8_t cr[4] = {block.r0, block.r1,
                   static_cast<uint8_t>(2.f / 3.f * block.r0 + 1.f / 3.f * block.r1),
                   static_cast<uint8_t>(1.f / 3.f * block.r0 + 2.f / 3.f * block.r1)};
  uint8_t cg[4] = {block.g0, block.g1,
                   static_cast<uint8_t>(2.f / 3.f * block.g0 + 1.f / 3.f * block.g1),
                   static_cast<uint8_t>(1.f / 3.f * block.g0 + 2.f / 3.f * block.g1)};

  auto output_bytes = static_cast<uint8_t*>(output);
  for (uint32_t oy = 0; oy < 4; ++oy) {
    for (uint32_t ox = 0; ox < 4; ++ox) {
      uint8_t xx = (block.xx >> (((ox + (oy * 4)) * 2))) & 3;
      output_bytes[(oy * length) + (ox * 2) + 0] = cr[xx];
      output_bytes[(oy * length) + (ox * 2) + 1] = cg[xx];
    }
  }
}

void ConvertTexelDXT3AToDXT3(xenos::Endian endian, void* output, const void* input, size_t length) {
  const uint32_t bytes_per_block = 16;
  auto output_bytes = static_cast<uint8_t*>(output);
  CopySwapBlock(endian, &output_bytes[0], input, 8);
  std::memset(&output_bytes[8], 0, 8);
}

// https://github.com/BinomialLLC/crunch/blob/ea9b8d8c00c8329791256adafa8cf11e4e7942a2/inc/crn_decomp.h#L4108
static uint32_t TiledOffset2DRow(uint32_t y, uint32_t width, uint32_t log2_bpp) {
  uint32_t macro = ((y / 32) * (width / 32)) << (log2_bpp + 7);
  uint32_t micro = ((y & 6) << 2) << log2_bpp;
  return macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2_bpp)) +
         ((y & 1) << 4);
}

static uint32_t TiledOffset2DColumn(uint32_t x, uint32_t y, uint32_t log2_bpp,
                                    uint32_t base_offset) {
  uint32_t macro = (x / 32) << (log2_bpp + 7);
  uint32_t micro = (x & 7) << log2_bpp;
  uint32_t offset = base_offset + (macro + ((micro & ~0xF) << 1) + (micro & 0xF));
  return ((offset & ~0x1FF) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

void Untile(uint8_t* output_buffer, const uint8_t* input_buffer, const UntileInfo* untile_info) {
  SCOPE_profile_cpu_f("gpu");
  assert_not_null(untile_info);
  assert_not_null(untile_info->input_format_info);
  assert_not_null(untile_info->output_format_info);

  uint32_t input_bytes_per_block = untile_info->input_format_info->bytes_per_block();
  uint32_t output_bytes_per_block = untile_info->output_format_info->bytes_per_block();
  uint32_t output_pitch = untile_info->output_pitch * output_bytes_per_block;

  // Bytes per pixel
  auto log2_bpp =
      (input_bytes_per_block / 4) + ((input_bytes_per_block / 2) >> (input_bytes_per_block / 4));

  // Offset to the current row, in bytes.
  uint32_t output_row_offset = 0;
  for (uint32_t y = 0; y < untile_info->height; y++) {
    auto input_row_offset =
        TiledOffset2DRow(untile_info->offset_y + y, untile_info->input_pitch, log2_bpp);

    // Go block-by-block on this row.
    uint32_t output_offset = output_row_offset;

    for (uint32_t x = 0; x < untile_info->width; x++) {
      auto input_offset = TiledOffset2DColumn(untile_info->offset_x + x, untile_info->offset_y + y,
                                              log2_bpp, input_row_offset);
      input_offset >>= log2_bpp;

      untile_info->copy_callback(&output_buffer[output_offset],
                                 &input_buffer[input_offset * input_bytes_per_block],
                                 output_bytes_per_block);

      output_offset += output_bytes_per_block;
    }

    output_row_offset += output_pitch;
  }
}

// ---------------------------------------------------------------------------
// CPU guest block -> host unit conversions (the Direct3D 11 load path).
// ---------------------------------------------------------------------------

namespace {

// Like CopySwapBlock, but byte modes that operate on 32-bit words keep the
// trailing bytes of a partial word instead of dropping them (a row of an odd
// number of small texels still needs its last texel).
void SwapEndianExact(xenos::Endian endian, void* output, const void* input, uint32_t bytes) {
  auto output_bytes = static_cast<uint8_t*>(output);
  const auto input_bytes = static_cast<const uint8_t*>(input);
  // Stutter fix: the no-swap case used to fall through to the trailing
  // byte-by-byte copy below, so every ConvertRunCopy (the common k_8_8_8_8 /
  // k_2_10_10_10 path) copied its whole run one byte at a time. A memcpy is
  // the identical copy, vectorized. Same for the 8-in-32 reverse: one word
  // bswap per 4 bytes instead of four byte moves (the compiler turns the
  // shift form into a single bswap; memcpy load/store keeps it safe for
  // unaligned run starts).
  if (endian == xenos::Endian::kNone) {
    std::memcpy(output, input, bytes);
    return;
  }
  uint32_t offset = 0;
  switch (endian) {
    case xenos::Endian::k8in16:
      for (; offset + 2 <= bytes; offset += 2) {
        output_bytes[offset] = input_bytes[offset + 1];
        output_bytes[offset + 1] = input_bytes[offset];
      }
      break;
    case xenos::Endian::k8in32:
      for (; offset + 4 <= bytes; offset += 4) {
        uint32_t word;
        std::memcpy(&word, input_bytes + offset, 4);
        word = ((word & 0x000000FFu) << 24) | ((word & 0x0000FF00u) << 8) |
               ((word & 0x00FF0000u) >> 8) | ((word & 0xFF000000u) >> 24);
        std::memcpy(output_bytes + offset, &word, 4);
      }
      break;
    case xenos::Endian::k16in32:
      for (; offset + 4 <= bytes; offset += 4) {
        output_bytes[offset] = input_bytes[offset + 2];
        output_bytes[offset + 1] = input_bytes[offset + 3];
        output_bytes[offset + 2] = input_bytes[offset];
        output_bytes[offset + 3] = input_bytes[offset + 1];
      }
      break;
    default:
    case xenos::Endian::kNone:
      break;
  }
  for (; offset < bytes; ++offset) {
    output_bytes[offset] = input_bytes[offset];
  }
}

uint16_t ReadLE16(const uint8_t* bytes) {
  return uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8);
}

uint32_t ReadLE32(const uint8_t* bytes) {
  return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) |
         (uint32_t(bytes[3]) << 24);
}

void WriteLE16(uint8_t* bytes, uint16_t value) {
  bytes[0] = uint8_t(value);
  bytes[1] = uint8_t(value >> 8);
}

uint64_t ReadLE64(const uint8_t* bytes) {
  uint64_t value = 0;
  for (uint32_t i = 0; i < 8; ++i) {
    value |= uint64_t(bytes[i]) << (8 * i);
  }
  return value;
}

// 16-bit texel shuffles: endian-swap the run in word-sized chunks (so the
// 32-bit swap modes stay exact even for long linear rows), then map each
// texel through the transform.
template <typename Transform>
void ConvertRun16Bit(xenos::Endian endian, void* output, uint32_t output_pitch, const void* input,
                     uint32_t input_bytes, Transform transform) {
  (void)output_pitch;
  uint8_t scratch[64];
  auto output_bytes = static_cast<uint8_t*>(output);
  const auto input_byte_ptr = static_cast<const uint8_t*>(input);
  for (uint32_t done = 0; done < input_bytes;) {
    uint32_t chunk = std::min(uint32_t(sizeof(scratch)), input_bytes - done);
    SwapEndianExact(endian, scratch, input_byte_ptr + done, chunk);
    for (uint32_t i = 0; i + 2 <= chunk; i += 2) {
      WriteLE16(output_bytes + done + i, transform(ReadLE16(scratch + i)));
    }
    if (chunk & 1) {
      // Never happens for 2-byte texels, but keep odd tails defined.
      output_bytes[done + chunk - 1] = scratch[chunk - 1];
    }
    done += chunk;
  }
}

}  // namespace

void ConvertRunCopy(xenos::Endian endian, void* output, uint32_t output_pitch, const void* input,
                    uint32_t input_bytes) {
  (void)output_pitch;
  SwapEndianExact(endian, output, input, input_bytes);
}

void ConvertRunR5G5B5A1ToB5G5R5A1(xenos::Endian endian, void* output, uint32_t output_pitch,
                                  const void* input, uint32_t input_bytes) {
  // Guest A1R5G5B5 (R in bits 14:10, B in 4:0) to host B5G5R5A1 (B in 14:10,
  // R in 4:0) - red and blue swapped, as the Direct3D 12 load shader does.
  ConvertRun16Bit(endian, output, output_pitch, input, input_bytes, [](uint16_t texel) {
    uint16_t r = (texel >> 10) & 0x1F;
    uint16_t g = (texel >> 5) & 0x1F;
    uint16_t b = texel & 0x1F;
    return uint16_t((texel & 0x8000) | (b << 10) | (g << 5) | r);
  });
}

void ConvertRunR5G6B5ToB5G6R5(xenos::Endian endian, void* output, uint32_t output_pitch,
                              const void* input, uint32_t input_bytes) {
  // Guest R5G6B5 (R in bits 15:11, B in 4:0) to host B5G6R5 (B in 15:11,
  // R in 4:0).
  ConvertRun16Bit(endian, output, output_pitch, input, input_bytes, [](uint16_t texel) {
    uint16_t r = (texel >> 11) & 0x1F;
    uint16_t g = (texel >> 5) & 0x3F;
    uint16_t b = texel & 0x1F;
    return uint16_t((b << 11) | (g << 5) | r);
  });
}

void ConvertRunRGBA4ToBGRA4(xenos::Endian endian, void* output, uint32_t output_pitch,
                            const void* input, uint32_t input_bytes) {
  // Guest R4G4B4A4 (R in bits 15:12, B in 7:4) to host B4G4R4A4 (B in 15:12,
  // R in 7:4).
  ConvertRun16Bit(endian, output, output_pitch, input, input_bytes, [](uint16_t texel) {
    uint16_t r = (texel >> 12) & 0xF;
    uint16_t g = (texel >> 8) & 0xF;
    uint16_t b = (texel >> 4) & 0xF;
    uint16_t a = texel & 0xF;
    return uint16_t((b << 12) | (g << 8) | (r << 4) | a);
  });
}

void ConvertRunDXT3AToR8(xenos::Endian endian, void* output, uint32_t output_pitch,
                         const void* input, uint32_t input_bytes) {
  // Guest DXT3A blocks are 16 2-bit alpha values (4x4 texels); the host is
  // R8_UNORM, so each texel becomes one byte. Writes 4 host rows per guest
  // block row.
  static const uint8_t kExpand2To8[4] = {0, 85, 170, 255};
  auto output_bytes = static_cast<uint8_t*>(output);
  const auto input_bytes_ptr = static_cast<const uint8_t*>(input);
  for (uint32_t block = 0; block < input_bytes / 8; ++block) {
    uint8_t block_bytes[8];
    SwapEndianExact(endian, block_bytes, input_bytes_ptr + block * 8, 8);
    uint64_t bits = ReadLE64(block_bytes);
    for (uint32_t y = 0; y < 4; ++y) {
      uint8_t* output_row = output_bytes + y * output_pitch + block * 4;
      for (uint32_t x = 0; x < 4; ++x) {
        output_row[x] = kExpand2To8[(bits >> ((y * 4 + x) * 2)) & 3];
      }
    }
  }
}

void ConvertRunCTX1ToR8G8(xenos::Endian endian, void* output, uint32_t output_pitch,
                          const void* input, uint32_t input_bytes) {
  // One guest block at a time through the per-texel converter - each block's
  // four rows are 8 bytes apart from the block to its right in the row below.
  auto output_bytes = static_cast<uint8_t*>(output);
  const auto input_bytes_ptr = static_cast<const uint8_t*>(input);
  for (uint32_t block = 0; block < input_bytes / 8; ++block) {
    ConvertTexelCTX1ToR8G8(endian, output_bytes + block * 8, input_bytes_ptr + block * 8,
                           output_pitch);
  }
}

void ConvertRunDepth24UnormToR32(xenos::Endian endian, void* output, uint32_t output_pitch,
                                 const void* input, uint32_t input_bytes) {
  (void)output_pitch;
  auto output_floats = static_cast<float*>(output);
  const auto input_bytes_ptr = static_cast<const uint8_t*>(input);
  for (uint32_t word = 0; word < input_bytes / 4; ++word) {
    uint8_t word_bytes[4];
    SwapEndianExact(endian, word_bytes, input_bytes_ptr + word * 4, 4);
    uint32_t packed = ReadLE32(word_bytes);
    output_floats[word] = UNorm24To32((packed >> 8) & 0xFFFFFF);
  }
}

void ConvertRunDepth24FloatToR32(xenos::Endian endian, void* output, uint32_t output_pitch,
                                 const void* input, uint32_t input_bytes) {
  (void)output_pitch;
  auto output_floats = static_cast<float*>(output);
  const auto input_bytes_ptr = static_cast<const uint8_t*>(input);
  for (uint32_t word = 0; word < input_bytes / 4; ++word) {
    uint8_t word_bytes[4];
    SwapEndianExact(endian, word_bytes, input_bytes_ptr + word * 4, 4);
    uint32_t packed = ReadLE32(word_bytes);
    output_floats[word] = Float20e4To32((packed >> 8) & 0xFFFFFF);
  }
}

void CopyGuestLevelSlice(const GuestLevelSlice& slice) {
  SCOPE_profile_cpu_f("gpu");
  assert_not_null(slice.source);
  assert_not_null(slice.destination);
  assert_not_null(slice.convert);
  const uint32_t block_bytes = slice.source_block_bytes;
  if (!slice.width_blocks || !slice.height_blocks || !slice.depth || !block_bytes) {
    return;
  }
  const uint32_t bytes_per_block_log2 = rex::log2_floor(block_bytes);
  const uint32_t row_pitch_bytes = slice.pitch_blocks * block_bytes;

  // The source address of one block of the slice, in bytes from source.
  // Tiled addresses are byte addresses already (the resolve and the texture
  // replacement paths use the same functions this way).
  auto block_source_offset = [&](uint32_t bx, uint32_t by, uint32_t bz) -> uint64_t {
    const uint32_t x = slice.offset_x_blocks + bx;
    const uint32_t y = slice.offset_y_blocks + by;
    const uint32_t z = slice.offset_z + bz;
    if (slice.tiled) {
      if (slice.tiled_3d) {
        return uint64_t(uint32_t(texture_util::GetTiledOffset3D(
            int32_t(x), int32_t(y), int32_t(z), slice.pitch_blocks,
            slice.height_blocks_storage, bytes_per_block_log2)));
      }
      return uint64_t(uint32_t(texture_util::GetTiledOffset2D(
          int32_t(x), int32_t(y), slice.pitch_blocks, bytes_per_block_log2)));
    }
    return uint64_t(z) * slice.z_slice_stride_block_rows * row_pitch_bytes +
           uint64_t(y) * row_pitch_bytes + uint64_t(x) * block_bytes;
  };

  // Stutter fix: for 2D-tiled slices the contiguous runs are exactly
  // clamp(16 >> bytes_per_block_log2, 1, 8) blocks long and aligned to
  // absolute-x multiples of that length (verified exhaustively against
  // GetTiledOffset2D across block sizes, pitches, rows and x offsets).
  // The old loop called GetTiledOffset2D once per block just to rediscover
  // those runs - ~13.5M calls for a 1280x720 4-byte texture. Compute the
  // offset once per run instead (4-8x fewer calls); the convert calls and
  // the bytes they move are identical. Linear, 3D-tiled and non-power-of-2
  // block sizes keep the original per-block loop below.
  const bool tiled_2d_fast_path =
      slice.tiled && !slice.tiled_3d && block_bytes == (uint32_t(1) << bytes_per_block_log2) &&
      bytes_per_block_log2 <= 4;
  if (tiled_2d_fast_path) {
    const uint32_t tiled_run_blocks =
        std::min(uint32_t(8), std::max(uint32_t(1), uint32_t(16) >> bytes_per_block_log2));
    for (uint32_t bz = 0; bz < slice.depth; ++bz) {
      for (uint32_t by = 0; by < slice.height_blocks; ++by) {
        uint8_t* destination_row = slice.destination +
                                   uint64_t(bz) * slice.destination_pitch_bytes *
                                           slice.destination_rows_per_block_row *
                                           slice.height_blocks +
                                   uint64_t(by) * slice.destination_pitch_bytes *
                                           slice.destination_rows_per_block_row;
        uint32_t bx = 0;
        while (bx < slice.width_blocks) {
          const uint32_t abs_x = slice.offset_x_blocks + bx;
          // Next run boundary past abs_x (a multiple of tiled_run_blocks).
          const uint32_t next_boundary =
              ((abs_x / tiled_run_blocks) + 1) * tiled_run_blocks;
          const uint32_t run_len =
              std::min(next_boundary - abs_x, slice.width_blocks - bx);
          const uint64_t run_offset = block_source_offset(bx, by, bz);
          slice.convert(slice.endian, destination_row + uint64_t(bx) *
                                                            slice.destination_block_row_bytes,
                        slice.destination_pitch_bytes, slice.source + run_offset,
                        run_len * block_bytes);
          bx += run_len;
        }
      }
    }
    return;
  }

  for (uint32_t bz = 0; bz < slice.depth; ++bz) {
    for (uint32_t by = 0; by < slice.height_blocks; ++by) {
      uint8_t* destination_row = slice.destination +
                                 uint64_t(bz) * slice.destination_pitch_bytes *
                                         slice.destination_rows_per_block_row *
                                         slice.height_blocks +
                                 uint64_t(by) * slice.destination_pitch_bytes *
                                         slice.destination_rows_per_block_row;
      // Group blocks whose source bytes are contiguous into one run - the
      // endianness swap modes operate on 32-bit words and must not be applied
      // across gaps (the 1-byte tiled layout has 8-block runs separated by
      // holes).
      uint32_t run_first = 0;
      uint64_t run_offset = block_source_offset(0, by, bz);
      for (uint32_t bx = 1; bx <= slice.width_blocks; ++bx) {
        uint64_t offset = bx < slice.width_blocks ? block_source_offset(bx, by, bz)
                                                  : UINT64_MAX;
        bool contiguous =
            bx < slice.width_blocks && offset == run_offset + uint64_t(bx - run_first) * block_bytes;
        if (!contiguous) {
          uint32_t run_blocks = bx - run_first;
          slice.convert(slice.endian, destination_row + uint64_t(run_first) *
                                                            slice.destination_block_row_bytes,
                        slice.destination_pitch_bytes, slice.source + run_offset,
                        run_blocks * block_bytes);
          run_first = bx;
          run_offset = offset;
        }
      }
    }
  }
}

}  // namespace rex::graphics::texture_conversion
