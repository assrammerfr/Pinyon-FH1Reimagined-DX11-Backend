/**
 * @file        graphics/d3d11/shader.cpp
 * @brief       D3D11 backend shader (Phase 3: real draws)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/shader.h>

#include <rex/logging.h>

#include <cstdio>

namespace rex::graphics::d3d11 {

uint64_t D3D11Shader::rejected_blob_count_ = 0;

D3D11Shader::D3D11Shader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
                         const uint32_t* ucode_dwords, size_t ucode_dword_count,
                         std::endian ucode_source_endian)
    : DxbcShader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
                 ucode_source_endian) {}

Shader::Translation* D3D11Shader::CreateTranslationInstance(uint64_t modification) {
  return new D3D11Translation(*this, modification);
}

bool D3D11Shader::D3D11Translation::CreateStage(ID3D11Device* device) {
  stage_creation_attempted_ = true;
  const std::vector<uint8_t>& binary = translated_binary();
  if (binary.empty()) {
    stage_create_failed_ = true;
    return false;
  }
  const xenos::ShaderType type = shader().type();
  HRESULT hr = E_FAIL;
  if (type == xenos::ShaderType::kVertex) {
    hr = device->CreateVertexShader(binary.data(), binary.size(), nullptr, &vertex_shader_);
  } else {
    hr = device->CreatePixelShader(binary.data(), binary.size(), nullptr, &pixel_shader_);
  }
  if (FAILED(hr)) {
    // Logged and counted by hash like the pipeline does after the SM 5.0
    // retarget - never silently retried, and never faked as valid.
    ++rejected_blob_count_;
    // Merge-night diagnostic: classify the rejection by the emitted shader
    // model. Scan the DXBC container for the SHEX chunk's version token:
    // 0xFFFE0500 = VS 5.0, 0xFFFE0501 = VS 5.1 (0xFFFF05xx pixel).
    uint32_t shex_version = 0, rdef_model = 0;
    const size_t dword_count = binary.size() / sizeof(uint32_t);
    const uint32_t* dwords = reinterpret_cast<const uint32_t*>(binary.data());
    if (dword_count > 8 && dwords[0] == 0x43425844u) {  // 'DXBC'
      const uint32_t chunk_count = dwords[7];
      for (uint32_t c = 0; c < chunk_count && size_t(8 + c) < dword_count; ++c) {
        const size_t off = size_t(dwords[8 + c]) / sizeof(uint32_t);
        if (off + 6 >= dword_count) {
          continue;
        }
        if (dwords[off] == 0x58454853u) {        // 'SHEX': [fourcc][size][version]
          shex_version = dwords[off + 2];
        } else if (dwords[off] == 0x46454452u) {  // 'RDEF': shader_model is data dword 4
          rdef_model = dwords[off + 6];
        }
      }
    }
    REXGPU_ERROR(
        "D3D11: device rejected translated {} shader {:016X} (modification {:016X}) - "
        "HRESULT 0x{:08X} (SHEX 0x{:08X}, RDEF model 0x{:08X}) ({} blobs rejected so far)",
        type == xenos::ShaderType::kVertex ? "vertex" : "pixel", shader().ucode_data_hash(),
        modification(), uint32_t(hr), shex_version, rdef_model, rejected_blob_count_);
    // Merge-night diagnostic part 2: dump the first few rejected blobs for
    // offline token inspection.
    static uint32_t dump_count = 0;
    if (dump_count < 3) {
      ++dump_count;
      char path[256];
      snprintf(path, sizeof(path), "F:\\PinyonShift\\work\\.local\\rejected_%s_%016llX.dxbc",
               type == xenos::ShaderType::kVertex ? "vs" : "ps",
               (unsigned long long)shader().ucode_data_hash());
      FILE* f = nullptr;
      fopen_s(&f, path, "wb");
      if (f) {
        fwrite(binary.data(), 1, binary.size(), f);
        fclose(f);
      }
    }
    stage_create_failed_ = true;
    return false;
  }
  return true;
}

ID3D11VertexShader* D3D11Shader::D3D11Translation::GetOrCreateVertexShader(ID3D11Device* device) {
  if (!stage_creation_attempted_ && !CreateStage(device)) {
    return nullptr;
  }
  return vertex_shader_.Get();
}

ID3D11PixelShader* D3D11Shader::D3D11Translation::GetOrCreatePixelShader(ID3D11Device* device) {
  if (!stage_creation_attempted_ && !CreateStage(device)) {
    return nullptr;
  }
  return pixel_shader_.Get();
}

}  // namespace rex::graphics::d3d11
