/**
 * @file        graphics/d3d11/fh1_native_resolve.cpp
 * @brief       D3D11 backend: FH1 native resolve path (Phase 4)
 *
 *              Mirrors the Direct3D 12 executor's ResolveToMemory constant
 *              packing and dispatch on the immediate context, from the SM
 *              5.0 compute bytecode (bytecode/d3d11_5_0). Phase 3 M2 adds the
 *              recorded form: the same plan appended to the command tape as a
 *              dispatch packet for replay through the immediate sink.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/fh1_native_resolve.h>

#include <cstring>

#include <rex/cvar.h>
#include <rex/graphics/d3d11/command_list.h>
#include <rex/graphics/d3d11/host_render_config.h>
#include <rex/graphics/flags.h>
#include <rex/logging.h>

namespace rex::graphics::d3d11 {

namespace shaders {
#include "../shaders/bytecode/d3d11_5_0/fh1_native_resolve_memory_color_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_resolve_memory_color_probe_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_resolve_memory_color_ms_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_resolve_memory_depth_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_resolve_memory_depth_ms_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_resolve_memory_uint_cs.h"
#include "../shaders/bytecode/d3d11_5_0/fh1_native_resolve_memory_uint_ms_cs.h"
}  // namespace shaders

namespace {

struct Bytecode {
  const void* code;
  size_t size;
};

// [source kind: color, depth, uint][source msaa].
const Bytecode kResolveMemoryShaders[3][2] = {
    {{shaders::fh1_native_resolve_memory_color_cs,
      sizeof(shaders::fh1_native_resolve_memory_color_cs)},
     {shaders::fh1_native_resolve_memory_color_ms_cs,
      sizeof(shaders::fh1_native_resolve_memory_color_ms_cs)}},
    {{shaders::fh1_native_resolve_memory_depth_cs,
      sizeof(shaders::fh1_native_resolve_memory_depth_cs)},
     {shaders::fh1_native_resolve_memory_depth_ms_cs,
      sizeof(shaders::fh1_native_resolve_memory_depth_ms_cs)}},
    {{shaders::fh1_native_resolve_memory_uint_cs,
      sizeof(shaders::fh1_native_resolve_memory_uint_cs)},
     {shaders::fh1_native_resolve_memory_uint_ms_cs,
      sizeof(shaders::fh1_native_resolve_memory_uint_ms_cs)}},
};

// Packed EDRAM surface layout for the resolve shaders (identical bits to the
// D3D12 and Vulkan executors, decoded in fh1_native_edram.hlsli).
uint32_t PackLayout(uint32_t base, uint32_t pitch, uint32_t msaa, bool is_64bpp, bool is_depth,
                    uint32_t format) {
  return (base & 0x7FF) | ((pitch & 0xFF) << 11) | ((msaa & 3) << 19) |
         (uint32_t(is_64bpp) << 21) | (uint32_t(is_depth) << 22) | ((format & 0xF) << 23);
}

// Source variant of a surface: 0 color, 1 depth and stencil, 2 raw color bits.
uint32_t SourceKind(bool is_depth, uint32_t format) {
  if (is_depth) return 1;
  return D3D11HostRenderConfig::GetColorUintDXGIFormat(xenos::ColorRenderTargetFormat(format)) !=
                 DXGI_FORMAT_UNKNOWN
             ? 2
             : 0;
}

// The guest rectangle in host pixels at the resolution scale (the executor's
// HostRect).
RECT HostRect(const RECT& rect, uint32_t scale) {
  return {rect.left * LONG(scale), rect.top * LONG(scale), rect.right * LONG(scale),
          rect.bottom * LONG(scale)};
}

}  // namespace

D3D11Fh1NativeResolve::D3D11Fh1NativeResolve(ID3D11Device* device,
                                             ID3D11DeviceContext* immediate_context,
                                             const D3D11HostRenderConfig& config, uint32_t scale)
    : device_(device), immediate_context_(immediate_context), config_(config), scale_(scale) {}

D3D11Fh1NativeResolve::~D3D11Fh1NativeResolve() {
  Shutdown();
}

void D3D11Fh1NativeResolve::Shutdown() {
  if (constants_buffer_) {
    constants_buffer_->Release();
    constants_buffer_ = nullptr;
  }
  if (constants_default_) {
    constants_default_->Release();
    constants_default_ = nullptr;
  }
  if (constants_snapshot_) {
    constants_snapshot_->Release();
    constants_snapshot_ = nullptr;
  }
  for (auto& by_msaa : resolve_memory_shaders_) {
    for (auto& shader : by_msaa) {
      if (shader) shader->Release();
      shader = nullptr;
    }
  }
}

bool D3D11Fh1NativeResolve::Skip(const char* reason) {
  ++skipped_count_;
  last_skip_ = reason;
  return false;
}

ID3D11ComputeShader* D3D11Fh1NativeResolve::GetResolveMemoryPipeline(uint32_t source_kind,
                                                                     bool msaa) {
  ID3D11ComputeShader*& shader = resolve_memory_shaders_[source_kind][msaa ? 1 : 0];
  if (shader) return shader;
  const Bytecode& bytecode = kResolveMemoryShaders[source_kind][msaa ? 1 : 0];
  if (FAILED(device_->CreateComputeShader(bytecode.code, bytecode.size, nullptr, &shader))) {
    shader = nullptr;
    return nullptr;
  }
  return shader;
}

ID3D11Buffer* D3D11Fh1NativeResolve::GetConstantsBuffer(bool update_mode) {
  ID3D11Buffer*& target = update_mode ? constants_default_ : constants_buffer_;
  if (target) return target;
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = kResolveMemoryConstantCount * sizeof(uint32_t);
  desc.Usage = update_mode ? D3D11_USAGE_DEFAULT : D3D11_USAGE_DYNAMIC;
  desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  if (!update_mode) desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(device_->CreateBuffer(&desc, nullptr, &target))) {
    target = nullptr;
    return nullptr;
  }
  return target;
}

// Diagnostic: copy a resolve's constant buffer into a CPU_READ snapshot and log
// it beside the CPU-packed values, to see what the GPU actually reads.
void D3D11Fh1NativeResolve::CaptureConstants(ID3D11Buffer* source, const uint32_t* cpu_values,
                                             uint32_t index) {
  if (!constants_snapshot_) {
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = kResolveMemoryConstantCount * sizeof(uint32_t);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device_->CreateBuffer(&desc, nullptr, &constants_snapshot_))) {
      constants_snapshot_ = nullptr;
      return;
    }
  }
  immediate_context_->CopyResource(constants_snapshot_, source);
  D3D11_MAPPED_SUBRESOURCE mapped;
  const HRESULT map_hr = immediate_context_->Map(constants_snapshot_, 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(map_hr)) {
    REXGPU_INFO("FH1 CB capture resolve={} failed map={:08X}", index, uint32_t(map_hr));
    return;
  }
  const uint32_t* gpu = static_cast<const uint32_t*>(mapped.pData);
  REXGPU_INFO("FH1 CB capture resolve={} cpu {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
              index, cpu_values[0], cpu_values[1], cpu_values[2], cpu_values[3], cpu_values[4],
              cpu_values[5], cpu_values[6], cpu_values[7]);
  REXGPU_INFO("FH1 CB capture resolve={} gpu {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
              index, gpu[0], gpu[1], gpu[2], gpu[3], gpu[4], gpu[5], gpu[6], gpu[7]);
  immediate_context_->Unmap(constants_snapshot_, 0);
}

// The shared front half of both resolve forms: pipeline selection (including
// the fh1_debug_resolve_store_probe variant), the constants buffer, the
// one-use source views, the destination adjustments and the packed constants.
// Nothing here touches the context's binding state.
bool D3D11Fh1NativeResolve::PlanResolveMemory(const SourceRect& source,
                                              const Fh1SurfaceKey& resolve_key,
                                              uint32_t sample_select, uint32_t dest_info,
                                              uint32_t dest_base, uint32_t dest_pitch,
                                              bool unscaled_dest, ResolveMemoryPlan& plan) {
  if (!source.surface) return Skip("resolve_owner_missing");
  D3D11Fh1NativeSurface& surface = *source.surface;
  const bool depth = surface.key.is_depth;
  const bool msaa = surface.samples > 1;
  ID3D11ComputeShader* pipeline =
      GetResolveMemoryPipeline(SourceKind(depth, surface.key.format), msaa);
  if (REXCVAR_GET(fh1_debug_resolve_store_probe) && !depth && !msaa) {
    if (!resolve_memory_probe_shader_ &&
        FAILED(device_->CreateComputeShader(shaders::fh1_native_resolve_memory_color_probe_cs,
                                            sizeof(shaders::fh1_native_resolve_memory_color_probe_cs),
                                            nullptr, &resolve_memory_probe_shader_))) {
      resolve_memory_probe_shader_ = nullptr;
    }
    if (resolve_memory_probe_shader_) {
      pipeline = resolve_memory_probe_shader_;
    }
  }
  if (!pipeline) return Skip("resolve_pipeline_create");
  const bool cb_update_mode = REXCVAR_GET(fh1_debug_resolve_cb_update);
  ID3D11Buffer* constants_buffer = GetConstantsBuffer(cb_update_mode);
  if (!constants_buffer) return Skip("resolve_constants_buffer");

  // One-use source views (the D3D12 executor uses one-use descriptors).
  for (uint32_t i = 0; i < (depth ? 2u : 1u); ++i) {
    const DXGI_FORMAT format = i ? surface.stencil_srv_format : surface.srv_format;
    D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
    desc.Format = format;
    if (msaa) {
      desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    } else {
      desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      desc.Texture2D.MipLevels = 1;
    }
    if (FAILED(device_->CreateShaderResourceView(surface.texture, &desc, &plan.srvs[i]))) {
      return Skip("resolve_source_view");
    }
  }
  if (!depth) plan.srvs[1] = plan.srvs[0];
  plan.srv_count = 2;

  // Diagnostic bisect: rewrite the destination pack class before it reaches
  // the shader, to separate "this pack's store is broken" from "this resolve
  // never dispatches a store". -1 keeps the guest's value.
  if (const int32_t forced_pack = REXCVAR_GET(fh1_debug_resolve_force_pack); forced_pack >= 0) {
    dest_info = (dest_info & ~7u) | uint32_t(forced_pack & 7);
  }
  // Diagnostic bisect of the destination's endianness bits (3:5) - the one
  // field that differs between resolves that land content and resolves that
  // land nothing on the same surface. -1 keeps the guest's value.
  if (const int32_t forced_endian = REXCVAR_GET(fh1_debug_resolve_force_endian);
      forced_endian >= 0) {
    dest_info = (dest_info & ~0x38u) | (uint32_t(forced_endian & 7) << 3);
  }

  const Fh1SurfaceKey& owner = surface.key;
  const uint32_t host_sample_mode =
      owner.msaa == uint32_t(xenos::MsaaSamples::k2X) ? (surface.samples == 4 ? 2u : 1u) : 0u;
  const RECT rect = unscaled_dest ? source.rect : HostRect(source.rect, scale_);
  plan.constants[0] = uint32_t(rect.left) | (uint32_t(rect.top) << 16);
  plan.constants[1] = uint32_t(rect.right - rect.left) | (uint32_t(rect.bottom - rect.top) << 16);
  plan.constants[2] = PackLayout(resolve_key.base_tiles, resolve_key.pitch_tiles, resolve_key.msaa,
                                 resolve_key.Is64bpp(), resolve_key.is_depth, resolve_key.format);
  plan.constants[3] = PackLayout(owner.base_tiles, owner.pitch_tiles, owner.msaa,
                                 owner.Is64bpp(), owner.is_depth, owner.format) |
                      (host_sample_mode << 27);
  plan.constants[4] = sample_select;
  plan.constants[5] = dest_info | ((scale_ - 1) << 20) | (unscaled_dest ? 1u << 22 : 0u);
  plan.constants[6] = dest_base;
  plan.constants[7] = dest_pitch;
  plan.pipeline = pipeline;
  plan.constants_buffer = constants_buffer;
  plan.cb_update_mode = cb_update_mode;
  plan.rect = rect;
  plan.dispatch_x = (uint32_t(rect.right - rect.left) + 7) / 8;
  plan.dispatch_y = (uint32_t(rect.bottom - rect.top) + 7) / 8;
  return true;
}

bool D3D11Fh1NativeResolve::ResolveToMemory(const SourceRect& source,
                                            const Fh1SurfaceKey& resolve_key,
                                            uint32_t sample_select, uint32_t dest_info,
                                            uint32_t dest_base, uint32_t dest_pitch,
                                            ID3D11UnorderedAccessView* memory_target,
                                            bool unscaled_dest) {
  if (!source.surface) return Skip("resolve_owner_missing");
  if (!memory_target) return Skip("resolve_memory_target");
  ResolveMemoryPlan plan;
  if (!PlanResolveMemory(source, resolve_key, sample_select, dest_info, dest_base, dest_pitch,
                         unscaled_dest, plan)) {
    return false;
  }
  D3D11Fh1NativeSurface& surface = *source.surface;
  const bool depth = surface.key.is_depth;

  // Direct3D 11 does not report a conflict between a resource bound as a shader
  // resource and the same resource left bound as a render target or
  // depth-stencil view in the output merger: it drops the shader resource bind
  // silently, and the compute shader then samples an unbound slot - zeros.
  // The draw path leaves the surface it just rendered to bound there, and the
  // resolve of that surface is exactly the one a frame presents, so this was
  // the blanking defect: with fh1_debug_resolve_bind_check, srv0 read back NULL
  // on the session's first resolve and on most later ones, while the same
  // resolve measured content in the source (source_in_rect 1800) and nothing
  // in the destination (0 of 942,080 words). The constant buffer was identical
  // CPU-packed and GPU-seen in those resolves, and the destination UAV and the
  // constant buffer binds survived. Unbind the output merger before binding
  // the source; every draw binds its targets again in
  // BindDrawRenderTargets, which is the only place that depends on them.
  {
    ID3D11RenderTargetView* no_render_targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    immediate_context_->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                           no_render_targets, nullptr);
  }

  uint32_t* constants = plan.constants;
  if (plan.cb_update_mode) {
    immediate_context_->UpdateSubresource(plan.constants_buffer, 0, nullptr, constants, 0, 0);
  } else {
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(immediate_context_->Map(plan.constants_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0,
                                       &mapped))) {
      return Skip("resolve_constants_map");
    }
    std::memcpy(mapped.pData, constants, sizeof(plan.constants));
    immediate_context_->Unmap(plan.constants_buffer, 0);
  }
  if (REXCVAR_GET(fh1_debug_resolve_cb_capture) && cb_capture_dispatches_ < 2) {
    CaptureConstants(plan.constants_buffer, constants, cb_capture_dispatches_);
    ++cb_capture_dispatches_;
  }

  ID3D11ShaderResourceView* srvs[2] = {plan.srvs[0].Get(), plan.srvs[1].Get()};
  UINT initial_count = 0;
  immediate_context_->CSSetShader(plan.pipeline, nullptr, 0);
  immediate_context_->CSSetConstantBuffers(0, 1, &plan.constants_buffer);
  immediate_context_->CSSetShaderResources(0, 2, srvs);
  immediate_context_->CSSetUnorderedAccessViews(0, 1, &memory_target, &initial_count);
  // Diagnostic (fh1_debug_resolve_bind_check): Direct3D 11 drops a view bind
  // silently when the resource is already bound as an incompatible view of
  // itself (a texture read through an SRV while it is the current render
  // target, for instance), and the dispatch then samples or stores nothing.
  // Read the bound state back from the context, before the dispatch, so the
  // log shows what the GPU will actually see rather than what was requested.
  if (REXCVAR_GET(fh1_debug_resolve_bind_check) &&
      (bind_checked_dispatches_ < 64 || (dispatch_count_ % 256u) == 0u)) {
    const uint32_t index = dispatch_count_;
    if (bind_checked_dispatches_ < 64) ++bind_checked_dispatches_;
    ID3D11ShaderResourceView* bound_srvs[2] = {nullptr, nullptr};
    immediate_context_->CSGetShaderResources(0, 2, bound_srvs);
    ID3D11UnorderedAccessView* bound_uav = nullptr;
    immediate_context_->CSGetUnorderedAccessViews(0, 1, &bound_uav);
    ID3D11Buffer* bound_cb = nullptr;
    immediate_context_->CSGetConstantBuffers(0, 1, &bound_cb);
    REXGPU_INFO(
        "FH1 resolve bind check: call={} srv0={} srv1={} uav={} cb={} info={:08X} "
        "owner={} rect={},{} {}x{} source={} target={}",
        index, bound_srvs[0] == srvs[0]   ? "ok"
               : (bound_srvs[0] ? "other" : "NULL"),
        depth ? (bound_srvs[1] == srvs[1] ? "ok" : (bound_srvs[1] ? "other" : "NULL"))
              : "same",
        bound_uav == memory_target ? "ok" : (bound_uav ? "other" : "NULL"),
        bound_cb == plan.constants_buffer ? "ok" : (bound_cb ? "other" : "NULL"), dest_info,
        surface.key.Describe(), plan.rect.left, plan.rect.top,
        uint32_t(plan.rect.right - plan.rect.left), uint32_t(plan.rect.bottom - plan.rect.top),
        uint64_t(uintptr_t(surface.texture)), uint64_t(uintptr_t(memory_target)));
    if (bound_srvs[0]) bound_srvs[0]->Release();
    if (bound_srvs[1]) bound_srvs[1]->Release();
    if (bound_uav) bound_uav->Release();
    if (bound_cb) bound_cb->Release();
  }
  immediate_context_->Dispatch(plan.dispatch_x, plan.dispatch_y, 1);
  if (REXCVAR_GET(fh1_swap_fetch_trace)) {
    static uint32_t resolve_dispatch_logs = 0;
    if (++resolve_dispatch_logs <= 40) {
      const uint32_t width = uint32_t(plan.rect.right - plan.rect.left);
      const uint32_t height = uint32_t(plan.rect.bottom - plan.rect.top);
      REXGPU_INFO(
          "FH1 resolve dispatch: shader={} (kind={} msaa={}) rect={},{} {}x{} groups={}x{} "
          "dest_base={:08X} dest_pitch={} dest_info={:08X} src_srv_format={} src_samples={} "
          "owner={}",
          plan.pipeline ==
                  resolve_memory_shaders_[SourceKind(depth, surface.key.format)][
                      surface.samples > 1 ? 1 : 0]
              ? "resolve_memory"
              : "other",
          SourceKind(depth, surface.key.format), surface.samples > 1, plan.rect.left,
          plan.rect.top, width, height, plan.dispatch_x, plan.dispatch_y, dest_base, dest_pitch,
          dest_info, uint32_t(surface.srv_format), surface.samples, surface.key.Describe());
    }
  }

  // Unbind so the views can be released and later binds don't alias.
  ID3D11UnorderedAccessView* null_uav = nullptr;
  ID3D11ShaderResourceView* null_srvs[2] = {nullptr, nullptr};
  immediate_context_->CSSetUnorderedAccessViews(0, 1, &null_uav, &initial_count);
  immediate_context_->CSSetShaderResources(0, 2, null_srvs);
  immediate_context_->CSSetShader(nullptr, nullptr, 0);

  ++dispatch_count_;
  return true;
}

bool D3D11Fh1NativeResolve::RecordResolveMemory(D3D11CommandList& tape, const SourceRect& source,
                                                const Fh1SurfaceKey& resolve_key,
                                                uint32_t sample_select, uint32_t dest_info,
                                                uint32_t dest_base, uint32_t dest_pitch,
                                                ID3D11UnorderedAccessView* memory_target,
                                                bool unscaled_dest) {
  if (!source.surface) return Skip("resolve_owner_missing");
  if (!memory_target) return Skip("resolve_memory_target");
  ResolveMemoryPlan plan;
  if (!PlanResolveMemory(source, resolve_key, sample_select, dest_info, dest_base, dest_pitch,
                         unscaled_dest, plan)) {
    return false;
  }
  D3D11Fh1NativeSurface& surface = *source.surface;
  const bool depth = surface.key.is_depth;

  // The constants reach the replay through the arena, not through a shared
  // buffer a later resolve would overwrite before the tape is submitted.
  D3D11ArenaSpan constants_span;
  if (!tape.AppendBytes(plan.constants, sizeof(plan.constants), constants_span)) {
    return false;
  }
  D3D11DispatchPacket* packet = tape.Append<D3D11DispatchPacket>();
  if (!packet) {
    return false;
  }
  packet->shader = plan.pipeline;
  packet->constant_buffers[0].buffer = plan.constants_buffer;
  packet->constant_buffers[0].data = constants_span;
  packet->srvs[0] = plan.srvs[0];
  packet->srvs[1] = plan.srvs[1];
  packet->srv_count = plan.srv_count;
  packet->uav = memory_target;
  packet->group_count_x = plan.dispatch_x;
  packet->group_count_y = plan.dispatch_y;
  packet->group_count_z = 1;
  packet->unbind_om = true;
  if (REXCVAR_GET(fh1_swap_fetch_trace)) {
    static uint32_t resolve_dispatch_logs = 0;
    if (++resolve_dispatch_logs <= 40) {
      const uint32_t width = uint32_t(plan.rect.right - plan.rect.left);
      const uint32_t height = uint32_t(plan.rect.bottom - plan.rect.top);
      REXGPU_INFO(
          "FH1 resolve dispatch (recorded): shader={} (kind={} msaa={}) rect={},{} {}x{} "
          "groups={}x{} dest_base={:08X} dest_pitch={} dest_info={:08X} src_srv_format={} "
          "src_samples={} owner={} packet={} arena={}",
          plan.pipeline ==
                  resolve_memory_shaders_[SourceKind(depth, surface.key.format)][
                      surface.samples > 1 ? 1 : 0]
              ? "resolve_memory"
              : "other",
          SourceKind(depth, surface.key.format), surface.samples > 1, plan.rect.left,
          plan.rect.top, width, height, plan.dispatch_x, plan.dispatch_y, dest_base, dest_pitch,
          dest_info, uint32_t(surface.srv_format), surface.samples, surface.key.Describe(),
          uint64_t(tape.command_count()), uint64_t(tape.arena_bytes()));
    }
  }
  ++dispatch_count_;
  return true;
}

}  // namespace rex::graphics::d3d11
