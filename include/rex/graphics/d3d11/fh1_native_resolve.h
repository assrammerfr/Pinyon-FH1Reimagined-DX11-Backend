/**
 * @file        graphics/d3d11/fh1_native_resolve.h
 * @brief       D3D11 backend: FH1 native resolve path (Phase 4)
 *
 *              Runs the single-source host resolve shaders (SM 5.0 compute,
 *              fh1_native_resolve_memory at cs_5_0) between the native EDRAM
 *              surfaces (fh1_native_surfaces.h) and the guest-memory mirror:
 *              each dispatch reads a surface rectangle through SRVs
 *              (Texture2DMS loads where the guest asks for MSAA samples) and
 *              writes the guest texture word layout into the mirror's raw
 *              byte-addressable UAV. The D3D12 executor's ResolveToMemory,
 *              on the immediate context.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

#include <d3d11.h>
#include <wrl/client.h>

#include <rex/graphics/d3d11/fh1_native_surfaces.h>
#include <rex/graphics/fh1_edram_surfaces.h>
#include <rex/ui/d3d11/d3d11_api.h>

namespace rex::graphics::d3d11 {

class D3D11CommandList;
class D3D11HostRenderConfig;

class D3D11Fh1NativeResolve {
 public:
  // Eight packed dwords of fh1_native_resolve_memory.cs.hlsl:
  // rect origin/size, two layout words, sample select, destination info, base
  // and pitch.
  static constexpr uint32_t kResolveMemoryConstantCount = 8;

  // Everything one resolve dispatch needs, prepared without touching the
  // immediate context (Phase 3 M2). The one-use source views are held by this
  // plan; the recorded path moves them into its packet and the immediate path
  // keeps them alive through the dispatch.
  struct ResolveMemoryPlan {
    ID3D11ComputeShader* pipeline = nullptr;
    ID3D11Buffer* constants_buffer = nullptr;
    bool cb_update_mode = false;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvs[2];
    uint32_t srv_count = 0;
    RECT rect = {};
    uint32_t dispatch_x = 0;
    uint32_t dispatch_y = 0;
    uint32_t constants[kResolveMemoryConstantCount] = {};
  };

  D3D11Fh1NativeResolve(ID3D11Device* device, ID3D11DeviceContext* immediate_context,
                        const D3D11HostRenderConfig& config, uint32_t scale);
  ~D3D11Fh1NativeResolve();

  void Shutdown();

  struct SourceRect {
    D3D11Fh1NativeSurface* surface = nullptr;
    RECT rect = {0, 0, 0, 0};  // Source pixels, guest (unscaled).
  };

  // One SM5.0 compute resolve of a source surface rectangle into the guest
  // texture word layout at dest_base / dest_pitch in the memory target (the
  // guest-memory mirror's raw UAV). Returns false with a named skip when the
  // dispatch cannot run.
  bool ResolveToMemory(const SourceRect& source, const Fh1SurfaceKey& resolve_key,
                       uint32_t sample_select, uint32_t dest_info, uint32_t dest_base,
                       uint32_t dest_pitch, ID3D11UnorderedAccessView* memory_target,
                       bool unscaled_dest = false);

  // Phase 3 M2: plans the same resolve and appends its dispatch packet to the
  // tape - constants in the tape arena, one-use source views as packet COM
  // references, unbind_om set so replay unbinds the output merger before the
  // source bind. No immediate-context call is made here. Returns false with a
  // named skip, or on tape overflow (the caller may then submit and retry).
  bool RecordResolveMemory(D3D11CommandList& tape, const SourceRect& source,
                           const Fh1SurfaceKey& resolve_key, uint32_t sample_select,
                           uint32_t dest_info, uint32_t dest_base, uint32_t dest_pitch,
                           ID3D11UnorderedAccessView* memory_target,
                           bool unscaled_dest = false);

  uint64_t dispatch_count() const { return dispatch_count_; }
  uint64_t skipped_count() const { return skipped_count_; }
  const char* last_skip() const { return last_skip_; }

 private:
  bool PlanResolveMemory(const SourceRect& source, const Fh1SurfaceKey& resolve_key,
                         uint32_t sample_select, uint32_t dest_info, uint32_t dest_base,
                         uint32_t dest_pitch, bool unscaled_dest,
                         ResolveMemoryPlan& plan_out);
  ID3D11ComputeShader* GetResolveMemoryPipeline(uint32_t source_kind, bool msaa);
  ID3D11Buffer* GetConstantsBuffer(bool update_mode);
  void CaptureConstants(ID3D11Buffer* source, const uint32_t* cpu_values, uint32_t index);
  bool Skip(const char* reason);

  ID3D11Device* device_;
  ID3D11DeviceContext* immediate_context_;
  const D3D11HostRenderConfig& config_;
  uint32_t scale_ = 1;

  // [source kind: color, depth, uint][source msaa].
  ID3D11ComputeShader* resolve_memory_shaders_[3][2] = {};
  // fh1_debug_resolve_store_probe: the color 1x shader built with FH1_STORE_PROBE,
  // which stores a marker word at UAV offset 0 before any guest data is read.
  ID3D11ComputeShader* resolve_memory_probe_shader_ = nullptr;
  ID3D11Buffer* constants_buffer_ = nullptr;
  // fh1_debug_resolve_cb_update: the same constants on a DEFAULT buffer written
  // by UpdateSubresource instead of Map/WRITE_DISCARD.
  ID3D11Buffer* constants_default_ = nullptr;
  // fh1_debug_resolve_cb_capture: CPU_READ snapshot of the constants as the
  // GPU saw them after the first two resolve dispatches of the session.
  ID3D11Buffer* constants_snapshot_ = nullptr;
  uint32_t cb_capture_dispatches_ = 0;
  // fh1_debug_resolve_bind_check: dispatches whose bound views were read back
  // from the context before the dispatch.
  uint32_t bind_checked_dispatches_ = 0;

  uint64_t dispatch_count_ = 0;
  uint64_t skipped_count_ = 0;
  const char* last_skip_ = nullptr;
};

}  // namespace rex::graphics::d3d11
