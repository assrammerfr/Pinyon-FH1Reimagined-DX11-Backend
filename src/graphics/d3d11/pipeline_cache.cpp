/**
 * @file        graphics/d3d11/pipeline_cache.cpp
 * @brief       D3D11 backend pipeline cache (Phase 3: real draws)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/pipeline_cache.h>
#include <cstdio>

#include <algorithm>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/d3d11/command_processor.h>
#include <rex/graphics/d3d11/graphics_system.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>
#include <rex/ui/d3d11/d3d11_provider.h>

namespace rex::graphics::d3d11 {

D3D11PipelineCache::D3D11PipelineCache(D3D11CommandProcessor& command_processor,
                                       const RegisterFile& register_file,
                                       const D3D11HostRenderConfig& host_config)
    : command_processor_(command_processor),
      register_file_(register_file),
      host_config_(host_config) {}

D3D11PipelineCache::~D3D11PipelineCache() { Shutdown(); }

bool D3D11PipelineCache::Initialize() {
  // Bindful mode: flat t/s/b registers (t0 = shared memory mirror, t1+ =
  // textures, s# = samplers, b0-b3 = constant buffers). No EDRAM ROV path on
  // feature level 11_0.
  // The translator's fourth parameter is gamma_render_target_as_unorm8, the
  // opposite of the host config's gamma_render_target_as_unorm16 - the same
  // inversion the Direct3D 12 pipeline cache applies (d3d12/pipeline_cache.cpp).
  // Passing the flag straight through made the shaders treat 8-bit gamma
  // render targets as 16-bit ones.
  shader_translator_ = std::make_unique<DxbcShaderTranslator>(
      command_processor_.GetD3D11Provider().GetAdapterVendorID(),
      /*bindless_resources_used=*/false, /*edram_rov_used=*/false,
      !host_config_.gamma_render_target_as_unorm16(), host_config_.msaa_2x_supported(),
      host_config_.draw_resolution_scale_x(), host_config_.draw_resolution_scale_y(),
      /*force_emit_source_map=*/false,
      // SM 5_0: the device is feature level 11_0 and rejects the SM 5_1
      // stamps the D3D12 mode emits (Step D contract - flat b/t/s/u slots).
      /*target_sm_50=*/true);
  return true;
}

void D3D11PipelineCache::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                 uint32_t title_id) {
  fh1_shader_pack_.Clear();
  fh1_shader_pack_hits_.store(0, std::memory_order_relaxed);
  fh1_shader_pack_misses_.store(0, std::memory_order_relaxed);
  // The Direct3D 12 and Vulkan backends gate the precompiled pack on the FH1
  // title ID; the pack holds this title's translated shaders only.
  if (title_id != 0x4D5309C9) {
    return;
  }
  const std::filesystem::path shareable_root = cache_root / "shaders" / "shareable";
  const Fh1ShaderPack::Config pack_config = Fh1ShaderPackConfig();
  const std::filesystem::path pack_path =
      shareable_root / Fh1ShaderPack::FileName(title_id, pack_config);
  // Opt-in: a pack's identity records the translator version, not the emitted
  // bytecode, so a pack built before a translator code-generation change still
  // loads - and this device refuses (or removes itself on binding) its
  // bytecode. Runtime translation stays the default until a current pack ships.
  if (!REXCVAR_GET(fh1_d3d11_shader_pack)) {
    std::error_code exists_error;
    if (std::filesystem::is_regular_file(pack_path, exists_error)) {
      REXGPU_INFO(
          "FH1 precompiled shader pack found but disabled (fh1_d3d11_shader_pack=false): {}",
          rex::path_to_utf8(pack_path));
    }
    return;
  }
  std::string pack_error;
  if (fh1_shader_pack_.Load(pack_path, pack_config, &pack_error)) {
    REXGPU_INFO(
        "Loaded {} FH1 precompiled shaders from {} (translator {:08X}, features {:02X}, flags "
        "{:02X}, {}x{})",
        fh1_shader_pack_.size(), rex::path_to_utf8(pack_path), pack_config.translator_version,
        pack_config.device_features, pack_config.flags, pack_config.draw_resolution_scale_x,
        pack_config.draw_resolution_scale_y);
  } else if (pack_error != "unavailable") {
    REXGPU_WARN("Ignoring FH1 precompiled shader pack {}: {}", rex::path_to_utf8(pack_path),
                pack_error);
  }
}

Fh1ShaderPack::Config D3D11PipelineCache::Fh1ShaderPackConfig() const {
  // Same flag encoding as the Direct3D 12 backend (fh1_shader_pack.h): bit 0
  // bindless resources, bit 2 gamma render targets as UNORM8, bit 3 native 2x
  // MSAA. The Direct3D 11 backend is always bindful, so bit 0 stays clear.
  uint32_t pack_flags = 0;
  pack_flags |= !host_config_.gamma_render_target_as_unorm16() ? 4u : 0u;
  pack_flags |= host_config_.msaa_2x_supported() ? 8u : 0u;
  return {DxbcShaderTranslator::Modification::kVersion, Fh1ShaderPack::Backend::kD3D11,
          Fh1ShaderPackDeviceFeatures(), pack_flags, host_config_.draw_resolution_scale_x(),
          host_config_.draw_resolution_scale_y()};
}

uint32_t D3D11PipelineCache::Fh1ShaderPackDeviceFeatures() const {
  // Switch statements for control flow are emitted except on Intel, where they
  // are slow - the same rule the Direct3D 12 backend applies to its packs.
  return command_processor_.GetD3D11Provider().GetAdapterVendorID() ==
                 ui::GraphicsProvider::GpuVendorID::kIntel
             ? 0u
             : Fh1ShaderPack::kD3D11FeatureSwitch;
}

void D3D11PipelineCache::LogPrecompiledShaderStats() const {
  if (!fh1_shader_pack_.size()) {
    return;
  }
  REXGPU_INFO(
      "FH1 precompiled shader cache: {} hits, {} misses, {} entries the device refused "
      "(misses and refusals translated at runtime)",
      fh1_shader_pack_hits_.load(std::memory_order_relaxed),
      fh1_shader_pack_misses_.load(std::memory_order_relaxed),
      fh1_shader_pack_rejected_.load(std::memory_order_relaxed));
}

void D3D11PipelineCache::Shutdown() {
  for (auto& it : pipelines_) {
    delete it.second;
  }
  pipelines_.clear();
  current_pipeline_ = nullptr;
  geometry_shaders_.clear();
  for (auto& it : shaders_) {
    delete it.second;
  }
  shaders_.clear();
  LogPrecompiledShaderStats();
  fh1_shader_pack_.Clear();
  fh1_shader_pack_hits_.store(0, std::memory_order_relaxed);
  fh1_shader_pack_misses_.store(0, std::memory_order_relaxed);
  fh1_shader_pack_rejected_.store(0, std::memory_order_relaxed);
  fh1_shader_pack_rejected_logs_ = 0;
  shader_translator_.reset();
}

D3D11Shader* D3D11PipelineCache::LoadShader(xenos::ShaderType shader_type,
                                            const uint32_t* host_address, uint32_t dword_count) {
  return LoadShader(shader_type, host_address, dword_count,
                    XXH3_64bits(host_address, dword_count * sizeof(uint32_t)));
}

D3D11Shader* D3D11PipelineCache::LoadShader(xenos::ShaderType shader_type,
                                            const uint32_t* host_address, uint32_t dword_count,
                                            uint64_t data_hash) {
  auto it = shaders_.find(data_hash);
  if (it != shaders_.end()) {
    return it->second;
  }
  D3D11Shader* shader = new D3D11Shader(shader_type, data_hash, host_address, dword_count);
  shaders_.emplace(data_hash, shader);
  return shader;
}

void D3D11PipelineCache::AnalyzeShaderUcode(Shader& shader) {
  // The D3D11 backend has no offline analysis catalog: analysis always runs
  // at load, on the command processor thread.
  if (shader.is_ucode_analyzed()) {
    return;
  }
  shader.AnalyzeUcode(ucode_disasm_buffer_);
}

DxbcShaderTranslator::Modification D3D11PipelineCache::GetCurrentVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t interpolator_mask) const {
  assert_true(shader.type() == xenos::ShaderType::kVertex);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;

  DxbcShaderTranslator::Modification modification;
  modification.vertex.dynamic_addressable_register_count =
      shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg);
  modification.vertex.host_vertex_shader_type = host_vertex_shader_type;
  modification.vertex.interpolator_mask = interpolator_mask;

  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes = pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  modification.vertex.user_clip_plane_count = rex::bit_count(user_clip_planes);
  modification.vertex.user_clip_plane_cull =
      uint32_t(user_clip_planes && pa_cl_clip_cntl.ucp_cull_only_ena);
  modification.vertex.point_ps_ucp_mode = pa_cl_clip_cntl.ps_ucp_mode;
  modification.vertex.vertex_kill_and = uint32_t(
      (shader.writes_point_size_edge_flag_kill_vertex() & 0b100) && !pa_cl_clip_cntl.vtx_kill_or);

  modification.vertex.output_point_size =
      uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
               regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);

  return modification;
}

DxbcShaderTranslator::Modification D3D11PipelineCache::GetCurrentPixelShaderModification(
    const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos,
    reg::RB_DEPTHCONTROL normalized_depth_control) const {
  assert_true(shader.type() == xenos::ShaderType::kPixel);
  assert_true(shader.is_ucode_analyzed());
  const auto& regs = register_file_;

  DxbcShaderTranslator::Modification modification;
  modification.pixel.dynamic_addressable_register_count =
      shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg);
  modification.pixel.interpolator_mask = interpolator_mask;
  modification.pixel.interpolators_centroid =
      interpolator_mask & ~xenos::GetInterpolatorSamplingPattern(
                              regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
                              regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                              regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);

  if (param_gen_pos < xenos::kMaxInterpolators) {
    modification.pixel.param_gen_enable = 1;
    modification.pixel.param_gen_interpolator = param_gen_pos;
    modification.pixel.param_gen_point =
        uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);
  } else {
    modification.pixel.param_gen_enable = 0;
    modification.pixel.param_gen_interpolator = 0;
    modification.pixel.param_gen_point = 0;
  }

  using DepthStencilMode = DxbcShaderTranslator::Modification::DepthStencilMode;
  if (host_config_.depth_float24_convert_in_pixel_shader() &&
      normalized_depth_control.z_enable &&
      regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    modification.pixel.depth_stencil_mode = host_config_.depth_float24_round()
                                                ? DepthStencilMode::kFloat24Rounding
                                                : DepthStencilMode::kFloat24Truncating;
  } else {
    if (shader.implicit_early_z_write_allowed() &&
        (!shader.writes_color_target(0) ||
         !draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>()))) {
      modification.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
    } else {
      modification.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
    }
  }

  return modification;
}

bool D3D11PipelineCache::TranslateShader(D3D11Shader::D3D11Translation& translation) {
  DxbcShaderTranslator* translator = shader_translator_.get();
  std::lock_guard<std::mutex> lock(translation_request_lock_);
  if (translation.is_translated()) {
    return translation.is_valid();
  }
  D3D11Shader& shader = static_cast<D3D11Shader&>(translation.shader());
  if (const Fh1ShaderPack::Entry* precompiled =
          fh1_shader_pack_.Find(shader.type(), shader.ucode_data_hash(), translation.modification())) {
    // Zero-filled: bindings are compared and hashed as bytes, padding too.
    std::vector<DxbcShader::TextureBinding> texture_bindings(precompiled->texture_bindings.size());
    std::memset(texture_bindings.data(), 0,
                texture_bindings.size() * sizeof(DxbcShader::TextureBinding));
    for (size_t i = 0; i < texture_bindings.size(); ++i) {
      const auto& binding = precompiled->texture_bindings[i];
      texture_bindings[i].bindless_descriptor_index = binding.bindless_descriptor_index;
      texture_bindings[i].fetch_constant = binding.fetch_constant;
      texture_bindings[i].dimension = binding.dimension;
      texture_bindings[i].is_signed = binding.is_signed;
    }
    std::vector<DxbcShader::SamplerBinding> sampler_bindings(precompiled->sampler_bindings.size());
    std::memset(sampler_bindings.data(), 0,
                sampler_bindings.size() * sizeof(DxbcShader::SamplerBinding));
    for (size_t i = 0; i < sampler_bindings.size(); ++i) {
      const auto& binding = precompiled->sampler_bindings[i];
      sampler_bindings[i].bindless_descriptor_index = binding.bindless_descriptor_index;
      sampler_bindings[i].fetch_constant = binding.fetch_constant;
      sampler_bindings[i].mag_filter = binding.mag_filter;
      sampler_bindings[i].min_filter = binding.min_filter;
      sampler_bindings[i].mip_filter = binding.mip_filter;
      sampler_bindings[i].aniso_filter = binding.aniso_filter;
    }
    // A pack built by a different translator state carries bytecode this
    // device may refuse outright. Offer it to the driver before adopting it, so
    // a stale pack degrades to runtime translation (logged) instead of failing
    // every draw - and never reaches a bind that could remove the device.
    ID3D11Device* device = command_processor_.GetD3D11Provider().GetDevice();
    HRESULT accept_hr = E_FAIL;
    if (shader.type() == xenos::ShaderType::kVertex) {
      Microsoft::WRL::ComPtr<ID3D11VertexShader> probe;
      accept_hr = device->CreateVertexShader(precompiled->bytecode.data(),
                                             precompiled->bytecode.size(), nullptr, &probe);
    } else {
      Microsoft::WRL::ComPtr<ID3D11PixelShader> probe;
      accept_hr = device->CreatePixelShader(precompiled->bytecode.data(),
                                            precompiled->bytecode.size(), nullptr, &probe);
    }
    if (SUCCEEDED(accept_hr)) {
      if (!shader.LoadPrecompiledBindings(texture_bindings, sampler_bindings,
                                          precompiled->used_texture_mask) ||
          !translation.LoadPrecompiledBinary(precompiled->bytecode)) {
        REXGPU_ERROR("FH1 precompiled shader {:016X} has incompatible runtime state",
                     shader.ucode_data_hash());
        return false;
      }
      fh1_shader_pack_hits_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    fh1_shader_pack_rejected_.fetch_add(1, std::memory_order_relaxed);
    if (fh1_shader_pack_rejected_logs_ < 8) {
      ++fh1_shader_pack_rejected_logs_;
      REXGPU_WARN(
          "D3D11: the device refused FH1 precompiled {} shader {:016X} (modification "
          "{:016X}, HRESULT 0x{:08X}) - translating it at runtime",
          shader.type() == xenos::ShaderType::kVertex ? "vertex" : "pixel",
          shader.ucode_data_hash(), translation.modification(), uint32_t(accept_hr));
    }
  } else {
    fh1_shader_pack_misses_.fetch_add(1, std::memory_order_relaxed);
  }
  if (!translator->TranslateAnalyzedShader(translation)) {
    REXGPU_ERROR("D3D11: shader {:016X} (modification {:016X}) translation failed",
                 translation.shader().ucode_data_hash(), translation.modification());
    return false;
  }
  if (!REXCVAR_GET(dump_shaders).empty()) {
    translation.Dump(REXCVAR_GET(dump_shaders), "d3d11");
  }
  return true;
}

bool D3D11PipelineCache::GetCurrentStateDescription(
    D3D11Shader::D3D11Translation* vertex_shader, D3D11Shader::D3D11Translation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
    uint32_t bound_depth_and_color_render_target_bits,
    const uint32_t* bound_depth_and_color_render_target_formats,
    PipelineDescription& description_out) {
  assert_true(vertex_shader->is_translated() && vertex_shader->is_valid());
  assert_true(!pixel_shader || (pixel_shader->is_translated() && pixel_shader->is_valid()));

  const auto& regs = register_file_;
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();

  std::memset(&description_out, 0, sizeof(description_out));

  bool tessellated = primitive_processing_result.IsTessellated();
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool rasterization_enabled = draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  if (!rasterization_enabled && pixel_shader) {
    // The pixel shader must be disabled externally before this call.
    return false;
  }

  // Vertex shader.
  description_out.vertex_shader_hash = vertex_shader->shader().ucode_data_hash();
  description_out.vertex_shader_modification = vertex_shader->modification();

  // Index buffer strip cut value (key parity with D3D12; Direct3D 11 has no
  // strip cut value state - the primitive processor handles reset).
  if (primitive_processing_result.host_primitive_reset_enabled) {
    description_out.strip_cut_index =
        primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16
            ? PipelineStripCutIndex::kFFFF
            : PipelineStripCutIndex::kFFFFFFFF;
  }

  // Host vertex shader type and primitive topology.
  if (!tessellated) {
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        description_out.primitive_topology_type = uint32_t(PipelinePrimitiveTopologyType::kPoint);
        break;
      case xenos::PrimitiveType::kLineList:
      case xenos::PrimitiveType::kLineStrip:
      // Quads are emulated as line lists with adjacency.
      case xenos::PrimitiveType::kQuadList:
      case xenos::PrimitiveType::k2DLineStrip:
        description_out.primitive_topology_type = uint32_t(PipelinePrimitiveTopologyType::kLine);
        break;
      default:
        description_out.primitive_topology_type = uint32_t(PipelinePrimitiveTopologyType::kTriangle);
        break;
    }
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        description_out.geometry_shader = PipelineGeometryShader::kPointList;
        break;
      case xenos::PrimitiveType::kRectangleList:
        description_out.geometry_shader = PipelineGeometryShader::kRectangleList;
        break;
      case xenos::PrimitiveType::kQuadList:
        description_out.geometry_shader = PipelineGeometryShader::kQuadList;
        break;
      default:
        description_out.geometry_shader = PipelineGeometryShader::kNone;
        break;
    }
  }
  if (!rasterization_enabled) {
    description_out.cull_mode = PipelineCullMode::kDisableRasterization;
    return true;
  }

  // Pixel shader.
  if (pixel_shader) {
    description_out.pixel_shader_hash = pixel_shader->shader().ucode_data_hash();
    description_out.pixel_shader_modification = pixel_shader->modification();
  }

  // Rasterizer state (per-side fill and depth bias collapsed the same way
  // Direct3D 12 requires - Direct3D 11 rasterizer state is also per-object).
  bool cull_front, cull_back;
  if (primitive_polygonal) {
    description_out.front_counter_clockwise = pa_su_sc_mode_cntl.face == 0;
    cull_front = pa_su_sc_mode_cntl.cull_front != 0;
    cull_back = pa_su_sc_mode_cntl.cull_back != 0;
    if (cull_front) {
      assert_false(cull_back);
      description_out.cull_mode = PipelineCullMode::kFront;
    } else if (cull_back) {
      description_out.cull_mode = PipelineCullMode::kBack;
    } else {
      description_out.cull_mode = PipelineCullMode::kNone;
    }
    if (!cull_front) {
      if (pa_su_sc_mode_cntl.polymode_front_ptype != xenos::PolygonType::kTriangles) {
        description_out.fill_mode_wireframe = 1;
      }
    }
    if (!cull_back) {
      if (pa_su_sc_mode_cntl.polymode_back_ptype != xenos::PolygonType::kTriangles) {
        description_out.fill_mode_wireframe = 1;
      }
    }
    if (pa_su_sc_mode_cntl.poly_mode != xenos::PolygonModeEnable::kDualMode) {
      description_out.fill_mode_wireframe = 0;
    }
  } else {
    cull_front = false;
    cull_back = false;
  }
  float polygon_offset, polygon_offset_scale;
  draw_util::GetPreferredFacePolygonOffset(regs, primitive_polygonal, polygon_offset_scale,
                                           polygon_offset);
  description_out.depth_bias = draw_util::GetD3D10IntegerPolygonOffset(
      regs.Get<reg::RB_DEPTH_INFO>().depth_format, polygon_offset);
  description_out.depth_bias_slope_scaled =
      polygon_offset_scale * xenos::kPolygonOffsetScaleSubpixelUnit;
  description_out.depth_clip = !regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;
  bool depth_stencil_bound_and_used = false;
  // Depth/stencil.
  if (bound_depth_and_color_render_target_bits & 1) {
    if (normalized_depth_control.z_enable) {
      description_out.depth_func = normalized_depth_control.zfunc;
      description_out.depth_write = normalized_depth_control.z_write_enable;
    } else {
      description_out.depth_func = xenos::CompareFunction::kAlways;
    }
    if (normalized_depth_control.stencil_enable) {
      description_out.stencil_enable = 1;
      bool stencil_backface_enable =
          primitive_polygonal && normalized_depth_control.backface_enable;
      Register stencil_ref_mask_reg;
      if (stencil_backface_enable && cull_front) {
        stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
      } else {
        stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK;
      }
      auto stencil_ref_mask = regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_reg);
      description_out.stencil_read_mask = stencil_ref_mask.stencilmask;
      description_out.stencil_write_mask = stencil_ref_mask.stencilwritemask;
      description_out.stencil_front_fail_op = normalized_depth_control.stencilfail;
      description_out.stencil_front_depth_fail_op = normalized_depth_control.stencilzfail;
      description_out.stencil_front_pass_op = normalized_depth_control.stencilzpass;
      description_out.stencil_front_func = normalized_depth_control.stencilfunc;
      if (stencil_backface_enable) {
        description_out.stencil_back_fail_op = normalized_depth_control.stencilfail_bf;
        description_out.stencil_back_depth_fail_op = normalized_depth_control.stencilzfail_bf;
        description_out.stencil_back_pass_op = normalized_depth_control.stencilzpass_bf;
        description_out.stencil_back_func = normalized_depth_control.stencilfunc_bf;
      } else {
        description_out.stencil_back_fail_op = description_out.stencil_front_fail_op;
        description_out.stencil_back_depth_fail_op = description_out.stencil_front_depth_fail_op;
        description_out.stencil_back_pass_op = description_out.stencil_front_pass_op;
        description_out.stencil_back_func = description_out.stencil_front_func;
      }
    }
    if (description_out.depth_func != xenos::CompareFunction::kAlways ||
        description_out.depth_write || description_out.stencil_enable) {
      description_out.depth_format =
          xenos::DepthRenderTargetFormat(bound_depth_and_color_render_target_formats[0]);
      depth_stencil_bound_and_used = true;
    }
  } else {
    description_out.depth_func = xenos::CompareFunction::kAlways;
  }

  // Render targets and blending state (blend factor maps identical to the
  // D3D12 backend's).
  static const PipelineBlendFactor kBlendFactorMap[32] = {
      /*  0 */ PipelineBlendFactor::kZero,
      /*  1 */ PipelineBlendFactor::kOne,
      /*  2 */ PipelineBlendFactor::kZero,  // ?
      /*  3 */ PipelineBlendFactor::kZero,  // ?
      /*  4 */ PipelineBlendFactor::kSrcColor,
      /*  5 */ PipelineBlendFactor::kInvSrcColor,
      /*  6 */ PipelineBlendFactor::kSrcAlpha,
      /*  7 */ PipelineBlendFactor::kInvSrcAlpha,
      /*  8 */ PipelineBlendFactor::kDestColor,
      /*  9 */ PipelineBlendFactor::kInvDestColor,
      /* 10 */ PipelineBlendFactor::kDestAlpha,
      /* 11 */ PipelineBlendFactor::kInvDestAlpha,
      /* 12 */ PipelineBlendFactor::kBlendFactor,
      /* 13 */ PipelineBlendFactor::kInvBlendFactor,
      /* 14 */ PipelineBlendFactor::kBlendFactor,
      /* 15 */ PipelineBlendFactor::kInvBlendFactor,
      /* 16 */ PipelineBlendFactor::kSrcAlphaSat,
  };
  static const PipelineBlendFactor kBlendFactorAlphaMap[32] = {
      /*  0 */ PipelineBlendFactor::kZero,
      /*  1 */ PipelineBlendFactor::kOne,
      /*  2 */ PipelineBlendFactor::kZero,  // ?
      /*  3 */ PipelineBlendFactor::kZero,  // ?
      /*  4 */ PipelineBlendFactor::kSrcAlpha,
      /*  5 */ PipelineBlendFactor::kInvSrcAlpha,
      /*  6 */ PipelineBlendFactor::kSrcAlpha,
      /*  7 */ PipelineBlendFactor::kInvSrcAlpha,
      /*  8 */ PipelineBlendFactor::kDestAlpha,
      /*  9 */ PipelineBlendFactor::kInvDestAlpha,
      /* 10 */ PipelineBlendFactor::kDestAlpha,
      /* 11 */ PipelineBlendFactor::kInvDestAlpha,
      /* 12 */ PipelineBlendFactor::kBlendFactor,
      /* 13 */ PipelineBlendFactor::kInvBlendFactor,
      /* 14 */ PipelineBlendFactor::kBlendFactor,
      /* 15 */ PipelineBlendFactor::kInvBlendFactor,
      /* 16 */ PipelineBlendFactor::kSrcAlphaSat,
  };
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (!(bound_depth_and_color_render_target_bits & (uint32_t(1) << (1 + i)))) {
      continue;
    }
    PipelineRenderTarget& rt = description_out.render_targets[i];
    rt.used = 1;
    rt.format = xenos::ColorRenderTargetFormat(bound_depth_and_color_render_target_formats[1 + i]);
    rt.write_mask = (normalized_color_mask >> (i * 4)) & 0xF;
    if (rt.write_mask) {
      auto blendcontrol =
          regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[i]);
      rt.src_blend = kBlendFactorMap[uint32_t(blendcontrol.color_srcblend)];
      rt.dest_blend = kBlendFactorMap[uint32_t(blendcontrol.color_destblend)];
      rt.blend_op = blendcontrol.color_comb_fcn;
      rt.src_blend_alpha = kBlendFactorAlphaMap[uint32_t(blendcontrol.alpha_srcblend)];
      rt.dest_blend_alpha = kBlendFactorAlphaMap[uint32_t(blendcontrol.alpha_destblend)];
      rt.blend_op_alpha = blendcontrol.alpha_comb_fcn;
    } else {
      rt.src_blend = PipelineBlendFactor::kOne;
      rt.dest_blend = PipelineBlendFactor::kZero;
      rt.blend_op = xenos::BlendOp::kAdd;
      rt.src_blend_alpha = PipelineBlendFactor::kOne;
      rt.dest_blend_alpha = PipelineBlendFactor::kZero;
      rt.blend_op_alpha = xenos::BlendOp::kAdd;
    }
  }
  xenos::MsaaSamples host_msaa_samples = regs.Get<reg::RB_SURFACE_INFO>().msaa_samples;
  if (!(bound_depth_and_color_render_target_bits & ~uint32_t(1)) &&
      !depth_stencil_bound_and_used) {
    host_msaa_samples = xenos::MsaaSamples::k1X;
  }
  description_out.host_msaa_samples = host_msaa_samples;

  return true;
}

D3D11_BLEND_DESC D3D11PipelineCache::GetBlendDesc(const PipelineDescription& description) {
  D3D11_BLEND_DESC desc = {};
  // Alpha to coverage is emulated in the pixel shader via the system
  // constants (alpha_to_mask), like on D3D12.
  desc.AlphaToCoverageEnable = FALSE;
  desc.IndependentBlendEnable = TRUE;
  auto to_d3d11_blend = [](PipelineBlendFactor factor) -> D3D11_BLEND {
    switch (factor) {
      case PipelineBlendFactor::kZero:
        return D3D11_BLEND_ZERO;
      case PipelineBlendFactor::kOne:
        return D3D11_BLEND_ONE;
      case PipelineBlendFactor::kSrcColor:
        return D3D11_BLEND_SRC_COLOR;
      case PipelineBlendFactor::kInvSrcColor:
        return D3D11_BLEND_INV_SRC_COLOR;
      case PipelineBlendFactor::kSrcAlpha:
        return D3D11_BLEND_SRC_ALPHA;
      case PipelineBlendFactor::kInvSrcAlpha:
        return D3D11_BLEND_INV_SRC_ALPHA;
      case PipelineBlendFactor::kDestColor:
        return D3D11_BLEND_DEST_COLOR;
      case PipelineBlendFactor::kInvDestColor:
        return D3D11_BLEND_INV_DEST_COLOR;
      case PipelineBlendFactor::kDestAlpha:
        return D3D11_BLEND_DEST_ALPHA;
      case PipelineBlendFactor::kInvDestAlpha:
        return D3D11_BLEND_INV_DEST_ALPHA;
      case PipelineBlendFactor::kBlendFactor:
        return D3D11_BLEND_BLEND_FACTOR;
      case PipelineBlendFactor::kInvBlendFactor:
        return D3D11_BLEND_INV_BLEND_FACTOR;
      default:
        return D3D11_BLEND_SRC_ALPHA_SAT;
    }
  };
  auto to_d3d11_blend_op = [](xenos::BlendOp op) -> D3D11_BLEND_OP {
    switch (op) {
      case xenos::BlendOp::kAdd:
        return D3D11_BLEND_OP_ADD;
      case xenos::BlendOp::kSubtract:
        return D3D11_BLEND_OP_SUBTRACT;
      case xenos::BlendOp::kRevSubtract:
        return D3D11_BLEND_OP_REV_SUBTRACT;
      case xenos::BlendOp::kMin:
        return D3D11_BLEND_OP_MIN;
      default:
        return D3D11_BLEND_OP_MAX;
    }
  };
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    D3D11_RENDER_TARGET_BLEND_DESC& rt = desc.RenderTarget[i];
    const PipelineRenderTarget& src = description.render_targets[i];
    if (!src.used || !src.write_mask) {
      rt.BlendEnable = FALSE;
      rt.SrcBlend = D3D11_BLEND_ONE;
      rt.DestBlend = D3D11_BLEND_ZERO;
      rt.BlendOp = D3D11_BLEND_OP_ADD;
      rt.SrcBlendAlpha = D3D11_BLEND_ONE;
      rt.DestBlendAlpha = D3D11_BLEND_ZERO;
      rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
      rt.RenderTargetWriteMask = 0;
      continue;
    }
    rt.BlendEnable =
        !(src.src_blend == PipelineBlendFactor::kOne && src.dest_blend == PipelineBlendFactor::kZero &&
          src.blend_op == xenos::BlendOp::kAdd &&
          src.src_blend_alpha == PipelineBlendFactor::kOne &&
          src.dest_blend_alpha == PipelineBlendFactor::kZero &&
          src.blend_op_alpha == xenos::BlendOp::kAdd);
    rt.SrcBlend = to_d3d11_blend(src.src_blend);
    rt.DestBlend = to_d3d11_blend(src.dest_blend);
    rt.BlendOp = to_d3d11_blend_op(src.blend_op);
    rt.SrcBlendAlpha = to_d3d11_blend(src.src_blend_alpha);
    rt.DestBlendAlpha = to_d3d11_blend(src.dest_blend_alpha);
    rt.BlendOpAlpha = to_d3d11_blend_op(src.blend_op_alpha);
    rt.RenderTargetWriteMask = src.write_mask;
  }
  return desc;
}

D3D11_DEPTH_STENCIL_DESC D3D11PipelineCache::GetDepthStencilDesc(
    const PipelineDescription& description) {
  D3D11_DEPTH_STENCIL_DESC desc = {};
  auto to_d3d11_comparison = [](xenos::CompareFunction func) -> D3D11_COMPARISON_FUNC {
    switch (func) {
      case xenos::CompareFunction::kNever:
        return D3D11_COMPARISON_NEVER;
      case xenos::CompareFunction::kLess:
        return D3D11_COMPARISON_LESS;
      case xenos::CompareFunction::kEqual:
        return D3D11_COMPARISON_EQUAL;
      case xenos::CompareFunction::kLessEqual:
        return D3D11_COMPARISON_LESS_EQUAL;
      case xenos::CompareFunction::kGreater:
        return D3D11_COMPARISON_GREATER;
      case xenos::CompareFunction::kNotEqual:
        return D3D11_COMPARISON_NOT_EQUAL;
      case xenos::CompareFunction::kGreaterEqual:
        return D3D11_COMPARISON_GREATER_EQUAL;
      default:
        return D3D11_COMPARISON_ALWAYS;
    }
  };
  auto to_d3d11_stencil_op = [](xenos::StencilOp op) -> D3D11_STENCIL_OP {
    switch (op) {
      case xenos::StencilOp::kKeep:
        return D3D11_STENCIL_OP_KEEP;
      case xenos::StencilOp::kZero:
        return D3D11_STENCIL_OP_ZERO;
      case xenos::StencilOp::kReplace:
        return D3D11_STENCIL_OP_REPLACE;
      case xenos::StencilOp::kIncrementClamp:
        return D3D11_STENCIL_OP_INCR_SAT;
      case xenos::StencilOp::kDecrementClamp:
        return D3D11_STENCIL_OP_DECR_SAT;
      case xenos::StencilOp::kInvert:
        return D3D11_STENCIL_OP_INVERT;
      case xenos::StencilOp::kIncrementWrap:
        return D3D11_STENCIL_OP_INCR;
      case xenos::StencilOp::kDecrementWrap:
        return D3D11_STENCIL_OP_DECR;
      default:
        return D3D11_STENCIL_OP_KEEP;
    }
  };
  const bool depth_used = description.depth_func != xenos::CompareFunction::kAlways ||
                          description.depth_write;
  desc.DepthEnable = depth_used ? TRUE : FALSE;
  desc.DepthWriteMask = description.depth_write ? D3D11_DEPTH_WRITE_MASK_ALL
                                                : D3D11_DEPTH_WRITE_MASK_ZERO;
  desc.DepthFunc = to_d3d11_comparison(description.depth_func);
  desc.StencilEnable = description.stencil_enable ? TRUE : FALSE;
  desc.StencilReadMask = UINT8(description.stencil_read_mask);
  desc.StencilWriteMask = UINT8(description.stencil_write_mask);
  desc.FrontFace.StencilFailOp = to_d3d11_stencil_op(description.stencil_front_fail_op);
  desc.FrontFace.StencilDepthFailOp = to_d3d11_stencil_op(description.stencil_front_depth_fail_op);
  desc.FrontFace.StencilPassOp = to_d3d11_stencil_op(description.stencil_front_pass_op);
  desc.FrontFace.StencilFunc = to_d3d11_comparison(description.stencil_front_func);
  desc.BackFace.StencilFailOp = to_d3d11_stencil_op(description.stencil_back_fail_op);
  desc.BackFace.StencilDepthFailOp = to_d3d11_stencil_op(description.stencil_back_depth_fail_op);
  desc.BackFace.StencilPassOp = to_d3d11_stencil_op(description.stencil_back_pass_op);
  desc.BackFace.StencilFunc = to_d3d11_comparison(description.stencil_back_func);
  return desc;
}

D3D11_RASTERIZER_DESC D3D11PipelineCache::GetRasterizerDesc(
    const PipelineDescription& description) {
  D3D11_RASTERIZER_DESC desc = {};
  desc.FillMode =
      description.fill_mode_wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
  switch (description.cull_mode) {
    case PipelineCullMode::kFront:
      desc.CullMode = D3D11_CULL_FRONT;
      break;
    case PipelineCullMode::kBack:
      desc.CullMode = D3D11_CULL_BACK;
      break;
    default:
      desc.CullMode = D3D11_CULL_NONE;
      break;
  }
  desc.FrontCounterClockwise = description.front_counter_clockwise ? TRUE : FALSE;
  desc.DepthBias = INT(description.depth_bias);
  desc.DepthBiasClamp = 0.0f;
  desc.SlopeScaledDepthBias = description.depth_bias_slope_scaled;
  desc.DepthClipEnable = description.depth_clip ? TRUE : FALSE;
  desc.ScissorEnable = TRUE;
  desc.MultisampleEnable = description.host_msaa_samples != xenos::MsaaSamples::k1X ? TRUE : FALSE;
  desc.AntialiasedLineEnable = TRUE;
  return desc;
}

ID3D11GeometryShader* D3D11PipelineCache::GetGeometryShader(
    PipelineGeometryShader geometry_shader_type,
    DxbcShaderTranslator::Modification vertex_shader_modification,
    DxbcShaderTranslator::Modification pixel_shader_modification) {
  if (geometry_shader_type == PipelineGeometryShader::kNone) {
    return nullptr;
  }
  GeometryShaderKey key;
  key.type = GeometryShaderType(uint32_t(geometry_shader_type));
  key.interpolator_count = rex::bit_count(vertex_shader_modification.vertex.interpolator_mask);
  key.user_clip_plane_count = vertex_shader_modification.vertex.user_clip_plane_count;
  key.user_clip_plane_cull = vertex_shader_modification.vertex.user_clip_plane_cull;
  key.has_vertex_kill_and = vertex_shader_modification.vertex.vertex_kill_and;
  key.has_point_size = vertex_shader_modification.vertex.output_point_size;
  key.has_point_coordinates = pixel_shader_modification.pixel.param_gen_point;
  key.point_ps_ucp_mode = vertex_shader_modification.vertex.point_ps_ucp_mode;
  auto it = geometry_shaders_.find(key);
  if (it != geometry_shaders_.end()) {
    return it->second.Get();
  }
  // Identical DXBC to the D3D12 backend's (shared assembler).
  std::vector<uint32_t> shader_dxbc;
  CreateDxbcGeometryShader(key, shader_dxbc, /*target_sm_50=*/true);
  Microsoft::WRL::ComPtr<ID3D11GeometryShader> shader;
  HRESULT gs_hr = command_processor_.GetD3D11Provider().GetDevice()->CreateGeometryShader(
      shader_dxbc.data(), shader_dxbc.size() * sizeof(uint32_t), nullptr, &shader);
  if (FAILED(gs_hr)) {
    REXGPU_ERROR("D3D11: failed to create geometry shader for key {:08X} (HRESULT 0x{:08X}, dxbc {} dwords)",
                 key.key, (uint32_t)gs_hr, (uint32_t)shader_dxbc.size());
    return nullptr;
  }
  it = geometry_shaders_.emplace(key, std::move(shader)).first;
  return it->second.Get();
}

D3D11PipelineCache::D3D11Pipeline* D3D11PipelineCache::ConfigurePipeline(
    D3D11Shader::D3D11Translation* vertex_shader, D3D11Shader::D3D11Translation* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
    uint32_t bound_depth_and_color_render_target_bits,
    const uint32_t* bound_depth_and_color_render_target_formats) {
  if (primitive_processing_result.IsTessellated()) {
    // Hull/domain shader translations are not part of Phase 3; dropped and
    // counted by the command processor.
    return nullptr;
  }
  // Translate synchronously on the command processor thread (no shader pack
  // until Step D).
  if (!vertex_shader->shader().is_ucode_analyzed()) {
    return nullptr;
  }
  if (!vertex_shader->is_translated() && !TranslateShader(*vertex_shader)) {
    return nullptr;
  }
  if (!vertex_shader->is_valid()) {
    return nullptr;
  }
  if (pixel_shader) {
    if (!pixel_shader->shader().is_ucode_analyzed()) {
      return nullptr;
    }
    if (!pixel_shader->is_translated() && !TranslateShader(*pixel_shader)) {
      return nullptr;
    }
    if (!pixel_shader->is_valid()) {
      return nullptr;
    }
  }

  PipelineDescription description;
  if (!GetCurrentStateDescription(vertex_shader, pixel_shader, primitive_processing_result,
                                  normalized_depth_control, normalized_color_mask,
                                  bound_depth_and_color_render_target_bits,
                                  bound_depth_and_color_render_target_formats, description)) {
    return nullptr;
  }
  uint64_t description_hash = XXH3_64bits(&description, sizeof(description));
  auto range = pipelines_.equal_range(description_hash);
  for (auto it = range.first; it != range.second; ++it) {
    if (!std::memcmp(&it->second->description, &description, sizeof(description))) {
      return it->second->state.get();
    }
  }

  ID3D11Device* device = command_processor_.GetD3D11Provider().GetDevice();
  auto pipeline = std::make_unique<Pipeline>();
  pipeline->description = description;
  pipeline->state = std::make_unique<D3D11Pipeline>();
  pipeline->state->description_hash = description_hash;
  // Lazy stage creation on the translation: a blob the device rejects is
  // logged and counted there, once, and never faked as valid.
  pipeline->state->vertex_shader = vertex_shader->GetOrCreateVertexShader(device);
  if (!pipeline->state->vertex_shader) {
    return nullptr;
  }
  if (pixel_shader) {
    pipeline->state->pixel_shader = pixel_shader->GetOrCreatePixelShader(device);
    if (!pipeline->state->pixel_shader) {
      return nullptr;
    }
  }
  if (description.geometry_shader != PipelineGeometryShader::kNone) {
    pipeline->state->geometry_shader = GetGeometryShader(
        description.geometry_shader,
        DxbcShaderTranslator::Modification(vertex_shader->modification()),
        DxbcShaderTranslator::Modification(pixel_shader ? pixel_shader->modification() : 0));
    if (!pipeline->state->geometry_shader) {
      return nullptr;
    }
  }
  const D3D11_BLEND_DESC blend_desc = GetBlendDesc(description);
  const D3D11_DEPTH_STENCIL_DESC depth_stencil_desc = GetDepthStencilDesc(description);
  const D3D11_RASTERIZER_DESC rasterizer_desc = GetRasterizerDesc(description);
  HRESULT state_hr = device->CreateBlendState(&blend_desc, &pipeline->state->blend_state);
  const char* failed_state = "blend";
  if (SUCCEEDED(state_hr)) {
    state_hr = device->CreateDepthStencilState(&depth_stencil_desc,
                                               &pipeline->state->depth_stencil_state);
    failed_state = "depth_stencil";
  }
  if (SUCCEEDED(state_hr)) {
    state_hr = device->CreateRasterizerState(&rasterizer_desc,
                                             &pipeline->state->rasterizer_state);
    failed_state = "rasterizer";
  }
  if (FAILED(state_hr)) {
    REXGPU_ERROR(
        "D3D11: failed to create pipeline state objects (hash {:016X}) failed_call={} "
        "hr={:08X}",
        description_hash, failed_state, static_cast<uint32_t>(state_hr));
    return nullptr;
  }
  D3D11Pipeline* result = pipeline->state.get();
  pipelines_.emplace(description_hash, pipeline.release());
  return result;
}

void D3D11PipelineCache::BindPipeline(const D3D11Pipeline& pipeline) {
  ID3D11DeviceContext* context = command_processor_.GetD3D11Provider().GetImmediateContext();
  context->VSSetShader(pipeline.vertex_shader.Get(), nullptr, 0);
  context->PSSetShader(pipeline.pixel_shader.Get(), nullptr, 0);
  context->GSSetShader(pipeline.geometry_shader.Get(), nullptr, 0);
  context->OMSetBlendState(pipeline.blend_state.Get(), nullptr, UINT32_MAX);
  context->OMSetDepthStencilState(pipeline.depth_stencil_state.Get(), 0);
  context->RSSetState(pipeline.rasterizer_state.Get());
}

}  // namespace rex::graphics::d3d11
