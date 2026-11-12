/**
 * @file        graphics/d3d11/pipeline_cache.h
 * @brief       D3D11 backend pipeline cache (Phase 3: real draws)
 *
 *              On Direct3D 11 there is no pipeline state object - this class
 *              is the state derivation + binding layer the D3D12 pipeline
 *              cache is for the D3D12 backend:
 *              - key semantics mirroring the D3D12 cache (shader hashes per
 *                stage + fixed-function state derived from the register file,
 *                PipelineDescription packed the same way),
 *              - per key it derives and caches ID3D11BlendState /
 *                ID3D11DepthStencilState / ID3D11RasterizerState plus the
 *                stage shader objects (D3D11Shader translations) and the
 *                geometry shader DXBC from the shared dxbc_geometry_shaders
 *                builder,
 *              - per draw it binds the stage objects and state objects to the
 *                immediate context.
 *
 *              Vertex fetch is in-shader: translated shaders read guest
 *              vertices from the shared-memory mirror SRV (t0), so there is
 *              no vertex-buffer binding path here - the primitive processor
 *              handles indices only.
 *
 *              Unlike the shipping D3D12 backend (FH1 shader pack), guest
 *              shaders are translated at runtime through the in-tree DXBC
 *              translator in its bindful mode (flat t/s/b registers), until
 *              the D3D11 shader pack lands in Step D.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/graphics/d3d11/host_render_config.h>
#include <rex/graphics/fh1_shader_pack.h>
#include <rex/graphics/d3d11/shader.h>
#include <rex/graphics/pipeline/shader/dxbc_geometry_shaders.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/primitive_processor.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/string/buffer.h>
#include <rex/ui/d3d11/d3d11_api.h>

namespace rex::graphics::d3d11 {

class D3D11CommandProcessor;

class D3D11PipelineCache {
 public:
  D3D11PipelineCache(D3D11CommandProcessor& command_processor, const RegisterFile& register_file,
                     const D3D11HostRenderConfig& host_config);
  ~D3D11PipelineCache();

  bool Initialize();
  void Shutdown();

  // Looks for the FH1 precompiled shader pack (`.pnsp`) in the shareable
  // shader storage under the cache root, for this backend's translator
  // version, device features, translation flags and resolution scale - the
  // same search the Direct3D 12 backend performs. A missing or incompatible
  // pack is not an error: every shader is translated at runtime instead.
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id);

  // Logs the precompiled pack's hit/miss/refused tallies. The title hard-exits
  // on termination, so Shutdown is not reached on a normal run: the command
  // processor also reports these with the periodic presentation line.
  void LogPrecompiledShaderStats() const;

  D3D11Shader* LoadShader(xenos::ShaderType shader_type, const uint32_t* host_address,
                          uint32_t dword_count);
  D3D11Shader* LoadShader(xenos::ShaderType shader_type, const uint32_t* host_address,
                          uint32_t dword_count, uint64_t data_hash);

  // Runtime translation needs ucode analysis on the command processor thread.
  void AnalyzeShaderUcode(Shader& shader);

  DxbcShaderTranslator::Modification GetCurrentVertexShaderModification(
      const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
      uint32_t interpolator_mask) const;
  DxbcShaderTranslator::Modification GetCurrentPixelShaderModification(
      const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
      reg::RB_DEPTHCONTROL normalized_depth_control) const;

  // A cached bundle of everything a draw binds: stage shader objects, the
  // geometry shader (if the host primitive type needs one), and the
  // fixed-function state objects. Created synchronously; nullptr on failure
  // (translation rejected, device refused a blob - logged with the hash).
  struct D3D11Pipeline {
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader;
    Microsoft::WRL::ComPtr<ID3D11GeometryShader> geometry_shader;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_stencil_state;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_state;
    uint64_t description_hash = 0;
  };

  // If draw_util::IsRasterizationPotentiallyDone is false, the pixel shader
  // MUST be made nullptr BEFORE calling this (same contract as D3D12).
  D3D11Pipeline* ConfigurePipeline(D3D11Shader::D3D11Translation* vertex_shader,
                                   D3D11Shader::D3D11Translation* pixel_shader,
                                   const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
                                   reg::RB_DEPTHCONTROL normalized_depth_control,
                                   uint32_t normalized_color_mask,
                                   uint32_t bound_depth_and_color_render_target_bits,
                                   const uint32_t* bound_depth_and_color_render_target_formats);

  // Binds a pipeline's shaders and state objects to the immediate context
  // (dynamic state - viewport, scissor, blend factor, stencil ref - stays
  // with the command processor).
  void BindPipeline(const D3D11Pipeline& pipeline);

 private:
  // Same packed key semantics as the D3D12 backend's PipelineDescription
  // (kept field-compatible on purpose so descriptions remain comparable
  // across backends in logs); only fields D3D11 can derive state from are
  // used for state objects.
  enum class PipelineStripCutIndex : uint32_t {
    kNone,
    kFFFF,
    kFFFFFFFF,
  };
  enum class PipelineGeometryShader : uint32_t {
    kNone,
    kPointList,
    kRectangleList,
    kQuadList,
  };
  enum class PipelineCullMode : uint32_t {
    kNone,
    kFront,
    kBack,
    kDisableRasterization,
  };
  enum class PipelineBlendFactor : uint32_t {
    kZero,
    kOne,
    kSrcColor,
    kInvSrcColor,
    kSrcAlpha,
    kInvSrcAlpha,
    kDestColor,
    kInvDestColor,
    kDestAlpha,
    kInvDestAlpha,
    kBlendFactor,
    kInvBlendFactor,
    kSrcAlphaSat,
  };
  enum class PipelinePrimitiveTopologyType : uint32_t {
    kPoint,
    kLine,
    kTriangle,
  };

  REXPACKEDSTRUCT(PipelineRenderTarget, {
    uint32_t used : 1;
    xenos::ColorRenderTargetFormat format : 4;
    PipelineBlendFactor src_blend : 4;
    PipelineBlendFactor dest_blend : 4;
    xenos::BlendOp blend_op : 3;
    PipelineBlendFactor src_blend_alpha : 4;
    PipelineBlendFactor dest_blend_alpha : 4;
    xenos::BlendOp blend_op_alpha : 3;
    uint32_t write_mask : 4;
  });

  REXPACKEDSTRUCT(PipelineDescription, {
    uint64_t vertex_shader_hash;
    uint64_t vertex_shader_modification;
    uint64_t pixel_shader_hash;
    uint64_t pixel_shader_modification;

    int32_t depth_bias;
    float depth_bias_slope_scaled;

    PipelineStripCutIndex strip_cut_index : 2;
    uint32_t primitive_topology_type : 2;
    PipelineGeometryShader geometry_shader : 2;
    uint32_t fill_mode_wireframe : 1;
    PipelineCullMode cull_mode : 2;
    uint32_t front_counter_clockwise : 1;
    uint32_t depth_clip : 1;
    xenos::MsaaSamples host_msaa_samples : 2;
    xenos::DepthRenderTargetFormat depth_format : 1;
    xenos::CompareFunction depth_func : 3;
    uint32_t depth_write : 1;
    uint32_t stencil_enable : 1;
    uint32_t stencil_read_mask : 8;

    uint32_t stencil_write_mask : 8;
    xenos::StencilOp stencil_front_fail_op : 3;
    xenos::StencilOp stencil_front_depth_fail_op : 3;
    xenos::StencilOp stencil_front_pass_op : 3;
    xenos::CompareFunction stencil_front_func : 3;
    xenos::StencilOp stencil_back_fail_op : 3;
    xenos::StencilOp stencil_back_depth_fail_op : 3;
    xenos::StencilOp stencil_back_pass_op : 3;
    xenos::CompareFunction stencil_back_func : 3;

    PipelineRenderTarget render_targets[xenos::kMaxColorRenderTargets];
  });

  // Derives the key + state parameters from the current register state.
  // Returns false if the draw cannot be represented (rasterization disabled
  // with a pixel shader bound, etc.).
  bool GetCurrentStateDescription(D3D11Shader::D3D11Translation* vertex_shader,
                                  D3D11Shader::D3D11Translation* pixel_shader,
                                  const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
                                  reg::RB_DEPTHCONTROL normalized_depth_control,
                                  uint32_t normalized_color_mask,
                                  uint32_t bound_depth_and_color_render_target_bits,
                                  const uint32_t* bound_depth_and_color_render_target_formats,
                                  PipelineDescription& description_out);

  // The geometry shader DXBC for a description, generated on demand with the
  // shared assembler (identical bytecode to the D3D12 backend's).
  ID3D11GeometryShader* GetGeometryShader(PipelineGeometryShader geometry_shader_type,
                                          DxbcShaderTranslator::Modification vertex_shader_modification,
                                          DxbcShaderTranslator::Modification pixel_shader_modification);

  // Synchronous translation of an analyzed shader. Precompiled pack entries
  // are used where the pack has them; runtime translation is the fallback for
  // hashes it misses (both counted, split logged at shutdown).
  bool TranslateShader(D3D11Shader::D3D11Translation& translation);

  // The pack identity a run requires (translator version, backend, device
  // features, translation flags, resolution scale) - the pack's file name and
  // header must match it exactly.
  Fh1ShaderPack::Config Fh1ShaderPackConfig() const;
  uint32_t Fh1ShaderPackDeviceFeatures() const;

  static D3D11_BLEND_DESC GetBlendDesc(const PipelineDescription& description);
  static D3D11_DEPTH_STENCIL_DESC GetDepthStencilDesc(const PipelineDescription& description);
  static D3D11_RASTERIZER_DESC GetRasterizerDesc(const PipelineDescription& description);

  D3D11CommandProcessor& command_processor_;
  const RegisterFile& register_file_;
  const D3D11HostRenderConfig& host_config_;

  // FH1 precompiled shader pack for this backend (empty if unavailable).
  Fh1ShaderPack fh1_shader_pack_;
  std::atomic<uint64_t> fh1_shader_pack_hits_{0};
  std::atomic<uint64_t> fh1_shader_pack_misses_{0};
  // Pack entries the device refused (bytecode from another translator state);
  // those translations fall back to runtime translation.
  std::atomic<uint64_t> fh1_shader_pack_rejected_{0};
  uint32_t fh1_shader_pack_rejected_logs_ = 0;

  // Runtime guest shader translation (bindful D3D11 mode - flat registers).
  std::unique_ptr<DxbcShaderTranslator> shader_translator_;
  std::mutex translation_request_lock_;
  // Temporary storage for ucode analysis on the command processor thread.
  string::StringBuffer ucode_disasm_buffer_;

  // Ucode hash -> shader.
  std::unordered_map<uint64_t, D3D11Shader*, rex::IdentityHasher<uint64_t>> shaders_;

  // Generated geometry shaders.
  std::unordered_map<GeometryShaderKey, Microsoft::WRL::ComPtr<ID3D11GeometryShader>,
                     GeometryShaderKey::Hasher>
      geometry_shaders_;

  struct Pipeline {
    // Null if creation failed (rejected blob, etc.) - the draw is dropped
    // and counted by the command processor.
    std::unique_ptr<D3D11Pipeline> state;
    PipelineDescription description;
  };
  std::unordered_multimap<uint64_t, Pipeline*, rex::IdentityHasher<uint64_t>> pipelines_;
  Pipeline* current_pipeline_ = nullptr;
};

}  // namespace rex::graphics::d3d11
