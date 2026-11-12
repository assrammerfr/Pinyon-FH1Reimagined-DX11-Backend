/**
 * @file        graphics/d3d11/shader.h
 * @brief       D3D11 backend shader (Phase 3: real draws)
 *
 *              Wraps a translated guest shader: the DXBC bytecode of the
 *              translation plus lazily created native stage objects. Mirrors
 *              the Direct3D 12 backend's D3D12Shader in shape; on Direct3D 11
 *              there is no pipeline state object, so the stage objects live
 *              on the shader translation itself and are shared by every
 *              pipeline (draw state) that uses the translation.
 *
 *              Stage objects are created on the command processor thread
 *              only. A blob the device rejects (such as an SM 5.1-stamped
 *              guest translation on feature level 11_0, until the SM 5.0
 *              retarget lands) is logged by shader hash once and counted;
 *              the translation then reports no stage object and the draw is
 *              dropped by the pipeline cache.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/ui/d3d11/d3d11_api.h>

namespace rex::graphics::d3d11 {

class D3D11Shader : public DxbcShader {
 public:
  class D3D11Translation : public DxbcTranslation {
   public:
    D3D11Translation(D3D11Shader& shader, uint64_t modification)
        : DxbcTranslation(shader, modification) {}

    // The native stage object for this translation, created from the
    // translated DXBC on first use. nullptr if the device rejected the blob
    // (logged and counted once per translation; never retried).
    // Command processor thread only.
    ID3D11VertexShader* GetOrCreateVertexShader(ID3D11Device* device);
    ID3D11PixelShader* GetOrCreatePixelShader(ID3D11Device* device);

    bool stage_creation_attempted() const { return stage_creation_attempted_; }

   private:
    bool CreateStage(ID3D11Device* device);

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
    bool stage_creation_attempted_ = false;
    bool stage_create_failed_ = false;
  };

  D3D11Shader(xenos::ShaderType shader_type, uint64_t ucode_data_hash, const uint32_t* ucode_dwords,
              size_t ucode_dword_count, std::endian ucode_source_endian = std::endian::big);

  // Counters of guest translation blobs the device refused (FL 11_0 vs SM 5.1
  // stamps until the Step D retarget), for the per-frame draw log.
  static uint64_t rejected_blob_count() { return rejected_blob_count_; }
  static void ResetRejectedBlobCount() { rejected_blob_count_ = 0; }

 protected:
  Translation* CreateTranslationInstance(uint64_t modification) override;

 private:
  static uint64_t rejected_blob_count_;
};

}  // namespace rex::graphics::d3d11
