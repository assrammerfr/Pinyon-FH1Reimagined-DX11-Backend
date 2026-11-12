/**
 * @file        graphics/d3d11/command_list_immediate_sink.cpp
 * @brief       Immediate-context replay sink for the D3D11 command tape (Phase 3 M1)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstring>
#include <memory>

#include <d3d11.h>
#include <wrl/client.h>

#include <rex/cvar.h>
#include <rex/graphics/d3d11/command_list.h>
#include <rex/graphics/flags.h>
#include <rex/logging.h>

namespace rex::graphics::d3d11 {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

class ImmediateCommandSink final : public D3D11CommandSink {
 public:
  explicit ImmediateCommandSink(ID3D11DeviceContext* context) : context_(context) {}

  void Execute(const D3D11Command& command, const D3D11CommandList& tape) override {
    std::visit(
        Overloaded{
            [](const D3D11BarrierPacket&) {},
            [](const D3D11UavBarrierPacket&) {},
            [this](const D3D11UnbindRenderTargetsPacket&) {
              context_->OMSetRenderTargets(0, nullptr, nullptr);
            },
            [this](const D3D11UnbindGraphicsSrvPacket&) {
              ID3D11ShaderResourceView* no_view = nullptr;
              context_->VSSetShaderResources(0, 1, &no_view);
              context_->PSSetShaderResources(0, 1, &no_view);
            },
            [this, &tape](const D3D11DispatchPacket& packet) { Dispatch(packet, tape); },
            [this](const D3D11CopyBufferPacket& packet) { CopyBuffer(packet); },
            [this](const D3D11CopyResourcePacket& packet) {
              if (packet.source && packet.destination) {
                context_->CopyResource(packet.destination.Get(), packet.source.Get());
              }
            },
            [this](const D3D11CopySubresourcePacket& packet) { CopySubresource(packet); },
            [this, &tape](const D3D11UploadBufferPacket& packet) { UploadBuffer(packet, tape); },
            [this, &tape](const D3D11UploadTexturePacket& packet) { UploadTexture(packet, tape); },
            [this](const D3D11ClearRenderTargetPacket& packet) {
              if (packet.render_target) {
                context_->ClearRenderTargetView(packet.render_target.Get(), packet.color.data());
              }
            },
            [this](const D3D11ClearDepthStencilPacket& packet) {
              if (packet.depth_stencil) {
                context_->ClearDepthStencilView(packet.depth_stencil.Get(), packet.flags,
                                                packet.depth, packet.stencil);
              }
            },
            [this](const D3D11ResolveSubresourcePacket& packet) {
              if (packet.source && packet.destination) {
                context_->ResolveSubresource(packet.destination.Get(),
                                             packet.destination_subresource, packet.source.Get(),
                                             packet.source_subresource, packet.format);
              }
            },
            [this, &tape](const D3D11DrawPacket& packet) { Draw(packet, tape); },
        },
        command);
  }

 private:
  // Diagnostic (fh1_debug_resolve_bind_check) on the recorded path: the same
  // read-back the immediate resolve does before its dispatch, so the on path
  // still proves the source views survived the reset-time binds (the blanking
  // defect was a silently dropped SRV, invisible without this).
  void CheckBindings(const D3D11DispatchPacket& packet) {
    ID3D11ShaderResourceView* bound_srvs[2] = {nullptr, nullptr};
    context_->CSGetShaderResources(0, 2, bound_srvs);
    ID3D11UnorderedAccessView* bound_uav = nullptr;
    context_->CSGetUnorderedAccessViews(0, 1, &bound_uav);
    ID3D11Buffer* bound_cb = nullptr;
    context_->CSGetConstantBuffers(0, 1, &bound_cb);
    const uint32_t srv1_index = packet.srv_count > 1 ? 1u : 0u;
    REXGPU_INFO(
        "FH1 resolve bind check: call={} srv0={} srv1={} uav={} cb={} groups={}x{} replay=1",
        replayed_dispatches_,
        bound_srvs[0] == packet.srvs[0].Get() ? "ok"
                                              : (bound_srvs[0] ? "other" : "NULL"),
        bound_srvs[1] == packet.srvs[srv1_index].Get()
            ? "ok"
            : (bound_srvs[1] ? "other" : "NULL"),
        bound_uav == packet.uav.Get() ? "ok" : (bound_uav ? "other" : "NULL"),
        bound_cb == packet.constant_buffers[0].buffer.Get()
            ? "ok"
            : (bound_cb ? "other" : "NULL"),
        packet.group_count_x, packet.group_count_y);
    if (bound_srvs[0]) bound_srvs[0]->Release();
    if (bound_srvs[1]) bound_srvs[1]->Release();
    if (bound_uav) bound_uav->Release();
    if (bound_cb) bound_cb->Release();
  }

  void UpdateConstantBuffer(const D3D11ConstantBufferBinding& binding,
                            const D3D11CommandList& tape) {
    if (!binding.buffer || binding.data.empty()) {
      return;
    }
    const uint8_t* data = tape.Data(binding.data);
    if (!data) {
      return;
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(context_->Map(binding.buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      std::memcpy(mapped.pData, data, binding.data.size);
      context_->Unmap(binding.buffer.Get(), 0);
    } else {
      D3D11_BOX box = {0, 0, 0, binding.data.size, 1, 1};
      context_->UpdateSubresource(binding.buffer.Get(), 0, &box, data, 0, 0);
    }
  }

  void Dispatch(const D3D11DispatchPacket& packet, const D3D11CommandList& tape) {
    if (packet.unbind_om) {
      context_->OMSetRenderTargets(0, nullptr, nullptr);
    }
    for (size_t i = 0; i < packet.constant_buffers.size(); ++i) {
      const D3D11ConstantBufferBinding& binding = packet.constant_buffers[i];
      if (!binding.buffer) {
        continue;
      }
      UpdateConstantBuffer(binding, tape);
      ID3D11Buffer* buffer = binding.buffer.Get();
      context_->CSSetConstantBuffers(UINT(i), 1, &buffer);
    }
    if (packet.shader) {
      context_->CSSetShader(packet.shader.Get(), nullptr, 0);
    }
    const uint32_t srv_count = packet.srv_count < 2 ? packet.srv_count : 2;
    if (srv_count) {
      ID3D11ShaderResourceView* srvs[2] = {packet.srvs[0].Get(), packet.srvs[1].Get()};
      context_->CSSetShaderResources(0, srv_count, srvs);
    }
    UINT initial_count = 0;
    if (packet.uav) {
      ID3D11UnorderedAccessView* uav = packet.uav.Get();
      context_->CSSetUnorderedAccessViews(0, 1, &uav, &initial_count);
    }
    if (REXCVAR_GET(fh1_debug_resolve_bind_check) &&
        (bind_checked_dispatches_ < 64 || (replayed_dispatches_ % 256u) == 0u)) {
      if (bind_checked_dispatches_ < 64) {
        ++bind_checked_dispatches_;
      }
      CheckBindings(packet);
    }
    context_->Dispatch(packet.group_count_x, packet.group_count_y, packet.group_count_z);
    ++replayed_dispatches_;
    // Leave no compute bindings behind: the next pass records its own state,
    // and the guest-memory copy may use the same views as inputs or outputs.
    ID3D11UnorderedAccessView* null_uav = nullptr;
    context_->CSSetUnorderedAccessViews(0, 1, &null_uav, &initial_count);
    ID3D11ShaderResourceView* null_srvs[2] = {nullptr, nullptr};
    context_->CSSetShaderResources(0, 2, null_srvs);
    context_->CSSetShader(nullptr, nullptr, 0);
  }

  void CopyBuffer(const D3D11CopyBufferPacket& packet) {
    if (!packet.source || !packet.destination || packet.length == 0) {
      return;
    }
    D3D11_BOX box = {packet.source_offset, 0, 0, packet.source_offset + packet.length, 1, 1};
    context_->CopySubresourceRegion(packet.destination.Get(), 0, packet.destination_offset, 0, 0,
                                    packet.source.Get(), 0, &box);
  }

  void CopySubresource(const D3D11CopySubresourcePacket& packet) {
    if (!packet.source || !packet.destination) {
      return;
    }
    const D3D11_BOX* box = nullptr;
    if (packet.box.right || packet.box.bottom || packet.box.back) {
      box = &packet.box;
    }
    context_->CopySubresourceRegion(packet.destination.Get(), packet.destination_subresource,
                                    packet.destination_x, packet.destination_y,
                                    packet.destination_z, packet.source.Get(),
                                    packet.source_subresource, box);
  }

  void UploadBuffer(const D3D11UploadBufferPacket& packet, const D3D11CommandList& tape) {
    if (!packet.destination || packet.data.empty()) {
      return;
    }
    const uint8_t* data = tape.Data(packet.data);
    if (!data) {
      return;
    }
    D3D11_BOX box = {
        packet.destination_offset, 0, 0, packet.destination_offset + packet.data.size, 1, 1};
    context_->UpdateSubresource(packet.destination.Get(), 0, &box, data, 0, 0);
  }

  void UploadTexture(const D3D11UploadTexturePacket& packet, const D3D11CommandList& tape) {
    if (!packet.destination || packet.data.empty()) {
      return;
    }
    const uint8_t* data = tape.Data(packet.data);
    if (!data) {
      return;
    }
    const bool has_box = packet.box.right || packet.box.bottom || packet.box.back;
    if (REXCVAR_GET(fh1_swap_fetch_trace) && packet.data.size >= (256u << 10)) {
      static uint32_t replay_logs = 0;
      if (replay_logs < 24) {
        ++replay_logs;
        REXGPU_INFO("FH1 draw tape upload replay: dest={} sub={} size={} row={} depth={} first={:08X}",
                    static_cast<const void*>(packet.destination.Get()),
                    packet.destination_subresource, packet.data.size, packet.row_pitch,
                    packet.depth_pitch, *reinterpret_cast<const uint32_t*>(data));
      }
    }
    context_->UpdateSubresource(packet.destination.Get(), packet.destination_subresource,
                                has_box ? &packet.box : nullptr, data, packet.row_pitch,
                                packet.depth_pitch);
  }

  // Phase 3 M3: the draw packet's constant-buffer bytes. The packet carries the
  // whole allocation (the record path pads the refresh to ByteWidth), so the
  // update is the same full-buffer UpdateSubresource the immediate path issues -
  // a boxed update was measured not to land for a 468-byte constant block on
  // this driver (the GPU kept zeros while the packet held the right dwords).
  void UploadDrawConstantBuffer(const D3D11DrawConstantBuffer& binding,
                                const D3D11CommandList& tape) {
    if (!binding.buffer || binding.data.empty()) {
      return;
    }
    const uint8_t* data = tape.Data(binding.data);
    if (!data) {
      return;
    }
    context_->UpdateSubresource(binding.buffer.Get(), 0, nullptr, data, 0, 0);
  }

  // Phase 3 M3: rebind a recorded draw and issue its one draw call. Every
  // binding is set from the packet - the tape never relies on state left by an
  // earlier packet or by the immediate path between submissions.
  void Draw(const D3D11DrawPacket& packet, const D3D11CommandList& tape) {
    for (const D3D11DrawConstantBuffer& binding : packet.constant_buffers) {
      UploadDrawConstantBuffer(binding, tape);
    }
    ID3D11Buffer* vs_cbs[4] = {};
    ID3D11Buffer* ps_cbs[4] = {};
    for (size_t i = 0; i < 4; ++i) {
      vs_cbs[i] = packet.vs_constant_buffers[i].Get();
      ps_cbs[i] = packet.ps_constant_buffers[i].Get();
    }
    context_->VSSetConstantBuffers(0, 4, vs_cbs);
    context_->PSSetConstantBuffers(0, 4, ps_cbs);
    context_->VSSetShader(packet.vertex_shader.Get(), nullptr, 0);
    context_->PSSetShader(packet.pixel_shader.Get(), nullptr, 0);
    context_->GSSetShader(packet.geometry_shader.Get(), nullptr, 0);
    const uint32_t vs_srv_count =
        packet.vs_srv_count < kMaxDrawTextureSrvs ? packet.vs_srv_count : kMaxDrawTextureSrvs;
    if (vs_srv_count) {
      ID3D11ShaderResourceView* srvs[kMaxDrawTextureSrvs] = {};
      for (uint32_t i = 0; i < vs_srv_count; ++i) {
        srvs[i] = packet.vs_srvs[i].Get();
      }
      context_->VSSetShaderResources(0, vs_srv_count, srvs);
    }
    const uint32_t ps_srv_count =
        packet.ps_srv_count < kMaxDrawTextureSrvs ? packet.ps_srv_count : kMaxDrawTextureSrvs;
    if (ps_srv_count) {
      ID3D11ShaderResourceView* srvs[kMaxDrawTextureSrvs] = {};
      for (uint32_t i = 0; i < ps_srv_count; ++i) {
        srvs[i] = packet.ps_srvs[i].Get();
      }
      context_->PSSetShaderResources(0, ps_srv_count, srvs);
    }
    const uint32_t vs_sampler_count =
        packet.vs_sampler_count < kMaxDrawSamplers ? packet.vs_sampler_count : kMaxDrawSamplers;
    if (vs_sampler_count) {
      ID3D11SamplerState* samplers[kMaxDrawSamplers] = {};
      for (uint32_t i = 0; i < vs_sampler_count; ++i) {
        samplers[i] = packet.vs_samplers[i].Get();
      }
      context_->VSSetSamplers(0, vs_sampler_count, samplers);
    }
    const uint32_t ps_sampler_count =
        packet.ps_sampler_count < kMaxDrawSamplers ? packet.ps_sampler_count : kMaxDrawSamplers;
    if (ps_sampler_count) {
      ID3D11SamplerState* samplers[kMaxDrawSamplers] = {};
      for (uint32_t i = 0; i < ps_sampler_count; ++i) {
        samplers[i] = packet.ps_samplers[i].Get();
      }
      context_->PSSetSamplers(0, ps_sampler_count, samplers);
    }
    const uint32_t render_target_count =
        packet.render_target_count < kMaxDrawRenderTargets ? packet.render_target_count
                                                           : kMaxDrawRenderTargets;
    if (render_target_count) {
      ID3D11RenderTargetView* rtvs[kMaxDrawRenderTargets] = {};
      for (uint32_t i = 0; i < render_target_count; ++i) {
        rtvs[i] = packet.render_targets[i].Get();
      }
      context_->OMSetRenderTargets(render_target_count, rtvs,
                                   packet.depth_stencil_view.Get());
    } else {
      context_->OMSetRenderTargets(0, nullptr, packet.depth_stencil_view.Get());
    }
    context_->OMSetBlendState(packet.blend_state.Get(), packet.blend_factor.data(),
                              packet.sample_mask);
    context_->OMSetDepthStencilState(packet.depth_stencil_state.Get(), packet.stencil_ref);
    context_->RSSetState(packet.rasterizer_state.Get());
    context_->RSSetViewports(1, &packet.viewport);
    context_->RSSetScissorRects(1, &packet.scissor);
    context_->IASetPrimitiveTopology(packet.topology);
    if (packet.indexed) {
      context_->IASetIndexBuffer(packet.index_buffer.Get(), packet.index_format,
                                 packet.index_buffer_offset);
      context_->DrawIndexed(packet.vertex_count, 0, 0);
    } else {
      context_->Draw(packet.vertex_count, 0);
    }
    ++replayed_draws_;
    replayed_vertices_ += packet.vertex_count;
    if (REXCVAR_GET(fh1_swap_fetch_trace) && (replayed_draws_ % 512u) == 0u) {
      REXGPU_INFO("FH1 draw tape replay: draws={} vertices={} last_vtx={} last_srvs={}/{}",
                  uint64_t(replayed_draws_), replayed_vertices_, packet.vertex_count,
                  packet.vs_srv_count, packet.ps_srv_count);
    }
    if (REXCVAR_GET(fh1_swap_fetch_trace) && !cb_readback_done_ && packet.vertex_count >= 512) {
      // One-shot: what the GPU actually holds in b0 after the packet's upload,
      // read back through a staging copy, next to the blob the packet carried.
      cb_readback_done_ = true;
      ID3D11Device* device = nullptr;
      context_->GetDevice(&device);
      Microsoft::WRL::ComPtr<ID3D11Buffer> staging;
      if (device && packet.constant_buffers[0].buffer) {
        D3D11_BUFFER_DESC desc = {};
        packet.constant_buffers[0].buffer->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(device->CreateBuffer(&desc, nullptr, &staging))) {
          context_->CopyResource(staging.Get(), packet.constant_buffers[0].buffer.Get());
          D3D11_MAPPED_SUBRESOURCE mapped = {};
          if (SUCCEEDED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            const uint32_t* words = static_cast<const uint32_t*>(mapped.pData);
            const uint8_t* blob = tape.Data(packet.constant_buffers[0].data);
            REXGPU_INFO(
                "FH1 draw tape CB check: draw={} size={} gpu_first={:08X},{:08X} blob_first={:08X},{:08X}",
                uint64_t(replayed_draws_), packet.constant_buffers[0].data.size, words[0],
                words[1], blob ? *reinterpret_cast<const uint32_t*>(blob) : 0u,
                blob && packet.constant_buffers[0].data.size >= 8
                    ? *reinterpret_cast<const uint32_t*>(blob + 4)
                    : 0u);
            context_->Unmap(staging.Get(), 0);
          }
        }
      }
      if (device) device->Release();
    }
    if (REXCVAR_GET(fh1_d3d11_command_list_validate)) {
      ValidateDraw(packet);
    }
  }

  // Diagnostic (fh1_d3d11_command_list_validate): read the just-bound state
  // back and compare it with the packet. A Direct3D 11 bind that is silently
  // dropped (the resolve path's blanking defect) would show here as a mismatch
  // rather than as a wrong frame. Reports the first mismatches, then a summary
  // every 512 draws.
  void ValidateDraw(const D3D11DrawPacket& packet) {
    uint32_t mismatches = 0;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    context_->VSGetShader(&vs, nullptr, nullptr);
    context_->PSGetShader(&ps, nullptr, nullptr);
    context_->GSGetShader(&gs, nullptr, nullptr);
    mismatches += vs != packet.vertex_shader.Get();
    mismatches += ps != packet.pixel_shader.Get();
    mismatches += gs != packet.geometry_shader.Get();
    if (vs) vs->Release();
    if (ps) ps->Release();
    if (gs) gs->Release();
    ID3D11Buffer* vs_cbs[4] = {};
    ID3D11Buffer* ps_cbs[4] = {};
    context_->VSGetConstantBuffers(0, 4, vs_cbs);
    context_->PSGetConstantBuffers(0, 4, ps_cbs);
    for (size_t i = 0; i < 4; ++i) {
      mismatches += vs_cbs[i] != packet.vs_constant_buffers[i].Get();
      mismatches += ps_cbs[i] != packet.ps_constant_buffers[i].Get();
      if (vs_cbs[i]) vs_cbs[i]->Release();
      if (ps_cbs[i]) ps_cbs[i]->Release();
    }
    const uint32_t vs_srv_count =
        packet.vs_srv_count < kMaxDrawTextureSrvs ? packet.vs_srv_count : kMaxDrawTextureSrvs;
    const uint32_t ps_srv_count =
        packet.ps_srv_count < kMaxDrawTextureSrvs ? packet.ps_srv_count : kMaxDrawTextureSrvs;
    if (vs_srv_count) {
      ID3D11ShaderResourceView* srvs[kMaxDrawTextureSrvs] = {};
      context_->VSGetShaderResources(0, vs_srv_count, srvs);
      for (uint32_t i = 0; i < vs_srv_count; ++i) {
        mismatches += srvs[i] != packet.vs_srvs[i].Get();
        if (srvs[i]) srvs[i]->Release();
      }
    }
    if (ps_srv_count) {
      ID3D11ShaderResourceView* srvs[kMaxDrawTextureSrvs] = {};
      context_->PSGetShaderResources(0, ps_srv_count, srvs);
      for (uint32_t i = 0; i < ps_srv_count; ++i) {
        mismatches += srvs[i] != packet.ps_srvs[i].Get();
        if (srvs[i]) srvs[i]->Release();
      }
    }
    const uint32_t vs_sampler_count =
        packet.vs_sampler_count < kMaxDrawSamplers ? packet.vs_sampler_count : kMaxDrawSamplers;
    const uint32_t ps_sampler_count =
        packet.ps_sampler_count < kMaxDrawSamplers ? packet.ps_sampler_count : kMaxDrawSamplers;
    if (vs_sampler_count) {
      ID3D11SamplerState* samplers[kMaxDrawSamplers] = {};
      context_->VSGetSamplers(0, vs_sampler_count, samplers);
      for (uint32_t i = 0; i < vs_sampler_count; ++i) {
        mismatches += samplers[i] != packet.vs_samplers[i].Get();
        if (samplers[i]) samplers[i]->Release();
      }
    }
    if (ps_sampler_count) {
      ID3D11SamplerState* samplers[kMaxDrawSamplers] = {};
      context_->PSGetSamplers(0, ps_sampler_count, samplers);
      for (uint32_t i = 0; i < ps_sampler_count; ++i) {
        mismatches += samplers[i] != packet.ps_samplers[i].Get();
        if (samplers[i]) samplers[i]->Release();
      }
    }
    const uint32_t render_target_count =
        packet.render_target_count < kMaxDrawRenderTargets ? packet.render_target_count
                                                           : kMaxDrawRenderTargets;
    ID3D11RenderTargetView* rtvs[kMaxDrawRenderTargets] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    if (render_target_count) {
      context_->OMGetRenderTargets(render_target_count, rtvs, &dsv);
      for (uint32_t i = 0; i < render_target_count; ++i) {
        mismatches += rtvs[i] != packet.render_targets[i].Get();
        if (rtvs[i]) rtvs[i]->Release();
      }
    } else {
      context_->OMGetRenderTargets(0, nullptr, &dsv);
    }
    mismatches += dsv != packet.depth_stencil_view.Get();
    if (dsv) dsv->Release();
    // The viewport and scissor carry the draw's screen placement; a silently
    // dropped RSSetViewports would move or shrink the geometry without any
    // other binding showing it.
    UINT viewport_count = 1;
    D3D11_VIEWPORT viewport = {};
    context_->RSGetViewports(&viewport_count, &viewport);
    UINT scissor_count = 1;
    D3D11_RECT scissor = {};
    context_->RSGetScissorRects(&scissor_count, &scissor);
    mismatches += viewport_count != 1;
    if (viewport_count == 1) {
      mismatches += viewport.TopLeftX != packet.viewport.TopLeftX;
      mismatches += viewport.TopLeftY != packet.viewport.TopLeftY;
      mismatches += viewport.Width != packet.viewport.Width;
      mismatches += viewport.Height != packet.viewport.Height;
      mismatches += viewport.MinDepth != packet.viewport.MinDepth;
      mismatches += viewport.MaxDepth != packet.viewport.MaxDepth;
    }
    mismatches += scissor_count != 1;
    if (scissor_count == 1) {
      mismatches += scissor.left != packet.scissor.left;
      mismatches += scissor.top != packet.scissor.top;
      mismatches += scissor.right != packet.scissor.right;
      mismatches += scissor.bottom != packet.scissor.bottom;
    }
    if (mismatches) {
      ++draw_validation_failures_;
      if (draw_validation_failures_ <= 8) {
        REXGPU_WARN(
            "D3D11 draw tape validation: draw={} mismatches={} srvs={}/{} samplers={}/{} "
            "targets={} indexed={} vp={},{} {}x{} scissor={},{} {}x{}",
            uint64_t(replayed_draws_), mismatches, packet.vs_srv_count, packet.ps_srv_count,
            packet.vs_sampler_count, packet.ps_sampler_count, packet.render_target_count,
            packet.indexed ? 1 : 0, uint32_t(packet.viewport.TopLeftX),
            uint32_t(packet.viewport.TopLeftY), uint32_t(packet.viewport.Width),
            uint32_t(packet.viewport.Height), packet.scissor.left, packet.scissor.top,
            packet.scissor.right, packet.scissor.bottom);
      }
    }
    if ((replayed_draws_ % 512u) == 0u) {
      REXGPU_INFO(
          "D3D11 draw tape validation: draws={} failures={} last={} indexed={} verts={} "
          "srvs={}/{} samplers={}/{} targets={} dsv={}",
          uint64_t(replayed_draws_), draw_validation_failures_, mismatches,
          packet.indexed ? 1 : 0, packet.vertex_count, packet.vs_srv_count,
          packet.ps_srv_count, packet.vs_sampler_count, packet.ps_sampler_count,
          packet.render_target_count, packet.depth_stencil_view ? 1 : 0);
    }
  }

  ID3D11DeviceContext* context_ = nullptr;
  uint64_t replayed_dispatches_ = 0;
  uint32_t bind_checked_dispatches_ = 0;
  uint64_t replayed_draws_ = 0;
  uint64_t replayed_vertices_ = 0;
  uint64_t draw_validation_failures_ = 0;
  bool cb_readback_done_ = false;
};

}  // namespace

std::unique_ptr<D3D11CommandSink> CreateD3D11ImmediateCommandSink(ID3D11DeviceContext* context) {
  return std::make_unique<ImmediateCommandSink>(context);
}

}  // namespace rex::graphics::d3d11
