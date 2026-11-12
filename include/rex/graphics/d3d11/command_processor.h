/**
 * @file        graphics/d3d11/command_processor.h
 * @brief       D3D11 backend command processor (Phase 3: real draws)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/d3d11/fh1_native_executor.h>
#include <rex/graphics/d3d11/fh1_native_resolve.h>
#include <rex/graphics/d3d11/fh1_native_surfaces.h>
#include <rex/graphics/d3d11/host_render_config.h>
#include <rex/graphics/d3d11/pipeline_cache.h>
#include <rex/graphics/d3d11/primitive_processor.h>
#include <rex/graphics/d3d11/render_target_cache.h>
#include <rex/graphics/d3d11/shared_memory.h>
#include <rex/graphics/d3d11/shader.h>
#include <rex/graphics/d3d11/texture_cache.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/ui/d3d11/d3d11_provider.h>

namespace rex::graphics {
struct Fh1ResolvePlan;
}  // namespace rex::graphics

namespace rex::graphics::d3d11 {

class D3D11GraphicsSystem;
class D3D11CommandList;
class D3D11CommandSink;
class D3D11ResourceStateTracker;
class D3D11SubmissionQueue;

class D3D11CommandProcessor : public CommandProcessor {
 public:
  D3D11CommandProcessor(D3D11GraphicsSystem* graphics_system, system::KernelState* kernel_state);
  ~D3D11CommandProcessor() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  ui::d3d11::D3D11Provider& GetD3D11Provider() const {
    return *static_cast<rex::ui::d3d11::D3D11Provider*>(graphics_system_->provider());
  }

  // Per-frame draw accounting for the gate harness log (issued, and dropped
  // with the reason that kept them from being issued).
  struct DrawCounters {
    uint64_t passes = 0;
    uint64_t issued = 0;
    uint64_t skipped_memexport = 0;
    uint64_t dropped_memexport = 0;
    uint64_t skipped_only_control = 0;
    uint64_t skipped_both_barriers = 0;
    uint64_t passes_in_color_depth_mode = 0;
    uint64_t passes_in_copy_mode_draw_paths = 0;
    uint64_t dropped_vshader_unbound = 0;
    uint64_t dropped_fspin_unbound = 0;
    uint64_t dropped_both_shaders_unbound = 0;
    uint64_t dropped_with_coherent_destination_in_copy_mode = 0;
    uint64_t dropped_screen_flipped_to_d24s8t = 0;
    uint64_t dropped_both_barriers_unset_at_draw_time = 0;
    uint64_t dropped_tier1_sample_patterns_at_draw_time = 0;
    uint64_t drawn = 0;
    uint64_t drawn_quads_in_color_depth_mode = 0;
    uint64_t dropped_quad_started_twice_in_same_pass = 0;
    uint64_t drawn_quad_started_twice_in_same_pass = 0;
    uint64_t dropped_poly_started_twice_in_same_pass = 0;
    uint64_t drawn_poly_started_twice_in_same_pass = 0;
    uint64_t dropped_invalid_poly_in_pass_with_quad_started = 0;
    uint64_t drawn_nonindex_passes = 0;
    uint64_t drawn_index_passes = 0;
    uint64_t dropped_all = 0;
    uint64_t dropped_other = 0;
    uint64_t dropped_tessellation = 0;
    uint64_t dropped_targets = 0;
    uint64_t dropped_shader = 0;
    uint64_t dropped_pipeline_state = 0;
    uint64_t dropped_bindings = 0;
  };
  const DrawCounters& draw_counters() const { return draw_counters_; }
  // Diagnostics only: the presented-frame index a log line belongs to.
  uint64_t presented_output_frames() const { return presented_output_frames_; }

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  // Hands the shareable shader storage root to the pipeline cache so it can
  // pick up the FH1 precompiled shader pack (runtime translation otherwise).
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking) override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;

 public:
  // Lands a guest range the resolve path wrote into the GPU mirror of shared
  // memory into guest memory, for a CPU-side reader of the same range (the
  // D3D11 texture cache untiles from guest memory). A resolve writes the mirror
  // only, so without this the range's pages stay GPU-written, the texture
  // upload's CopyCpuRange refuses them, and the guest samples the texture it
  // just resolved as if it held nothing - the 3D scene never reaches the
  // composite that reads it. Returns true if the range had GPU-written pages
  // and was refreshed.
  bool RefreshResolvedGuestRange(uint32_t start, uint32_t length);

  // Phase 3 M3: a texture-cache load's upload as an UploadTexture packet. Only
  // records while a draw's RequestTextures is running; the swap's own upload
  // stays immediate. False means the caller must upload it itself (recording
  // off, or the tape overflowed twice).
  bool RecordTextureUpload(ID3D11Resource* destination, uint32_t destination_subresource,
                           const void* data, uint32_t size, uint32_t row_pitch,
                           uint32_t depth_pitch);

 private:
  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  // Fetch constant writes must reach the texture cache so it re-derives the
  // changed bindings (mirrors the Direct3D 12 command processor).
  void WriteRegister(uint32_t index, uint32_t value) override;
  void WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                             uint32_t num_registers) override;

  // Binds the FH1 native EDRAM surfaces the draw writes (derived from the
  // registers the way the guest addresses EDRAM) and reports the bound bits
  // and host formats for the pipeline key. Full EDRAM tile ownership is
  // Step B/C; this binds what exists.
  // `claim_x0`..`claim_y1`: the rectangle the draw can write (viewport and
  // scissor intersected, guest pixels), which the executor claims EDRAM tiles
  // for.
  bool BindDrawRenderTargets(bool rasterization_done,
                             reg::RB_DEPTHCONTROL normalized_depth_control,
                             uint32_t normalized_color_mask, int32_t claim_x0, int32_t claim_y0,
                             int32_t claim_x1, int32_t claim_y1,
                             uint32_t& bound_depth_and_color_render_target_bits_out,
                             uint32_t* bound_depth_and_color_render_target_formats_out);

  void UpdateFixedFunctionState(const draw_util::ViewportInfo& viewport_info,
                                const draw_util::Scissor& scissor, bool primitive_polygonal,
                                reg::RB_DEPTHCONTROL normalized_depth_control);
  void UpdateSystemConstantValues(bool shared_memory_is_uav, bool primitive_polygonal,
                                  uint32_t line_loop_closing_index, xenos::Endian index_endian,
                                  const draw_util::ViewportInfo& viewport_info,
                                  uint32_t used_texture_mask,
                                  reg::RB_DEPTHCONTROL normalized_depth_control,
                                  uint32_t normalized_color_mask);
  bool UpdateBindings(D3D11Shader* vertex_shader, D3D11Shader* pixel_shader);
  bool EnsureVertexBufferResidency(const Shader::ConstantRegisterMap& constant_map_vertex);

  // Constant buffer kept up to date lazily (invalidated by register writes).
  // `cpu_bytes` is the payload of the last update: on the record path the
  // update is not written to the GPU at record time (Phase 3 M3), the packet
  // carries these bytes and the replay writes them before the draw, so a
  // packet never depends on the buffer's contents between submissions.
  struct ConstantBufferBinding {
    bool up_to_date = false;
    uint32_t size = 0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    std::vector<uint8_t> cpu_bytes;
  };
  bool UpdateConstantBuffer(ConstantBufferBinding& binding, const void* data, uint32_t size_bytes);

  // Blocks until every command submitted so far has executed. This backend has
  // no submission fences, and presentation reads the frontbuffer's bytes on the
  // CPU, so the swap needs one real completion point.
  bool WaitForGpu();
  // Reports the accumulated swap wait (mean per swap) - periodically, and at
  // shutdown.
  void LogSwapWait();
  // Phase 3 M3: the tape's accounting - packets, arena bytes, submissions, and
  // for the draw path how many draws were recorded, how many stayed immediate,
  // and how many texture uploads the cache put on the tape. Periodic and at
  // shutdown, so a hard-exiting title still reports it.
  void LogTapeCounters();

  // Phase 3 M2: the recorded resolve/transfer path. RecordResolveBatch appends
  // the batch's unbind, transitions and dispatch; RecordResolveStagingCopy
  // appends the staging-to-mirror buffer copy; SubmitFrameTape replays the
  // open tape through the immediate sink and resets it. Submission is at the
  // resolve boundary: the draws that follow in the same frame are still
  // immediate, so holding the batch until the swap would run it after them and
  // change the order the A/B measurements compare (the frame-boundary
  // submission lands with M3/M4, when draws and the queue join the tape).
  bool RecordResolveBatch(const Fh1ResolvePlan& plan, D3D11Fh1NativeSurface* source_surface,
                          ID3D11UnorderedAccessView* memory_target, bool use_staging_copy);
  bool RecordResolveStagingCopy(uint32_t host_offset, uint32_t length);
  bool SubmitFrameTape(const char* reason);

  // Phase 3 M3: the draw path on the tape. RecordDrawPacket captures the state
  // the draw has just bound on the immediate context (shaders, state objects,
  // constant buffers with their record-time bytes, views, samplers, targets,
  // viewport/scissor, topology, index buffer) and appends it with the draw
  // arguments; replay rebinds all of it and issues the one draw. Returns false
  // when the draw cannot be recorded (packet/arena cap, a binding the packet
  // cannot hold) - the caller then issues it immediately. If a retry after an
  // overflow also fails, the deferred constant-buffer bytes are written before
  // returning, so the immediate draw sees exactly what it would have written.
  bool RecordDrawPacket(const D3D11PipelineCache::D3D11Pipeline& pipeline,
                        D3D11_PRIMITIVE_TOPOLOGY topology, uint32_t vertex_count, bool indexed,
                        ID3D11Buffer* index_buffer, DXGI_FORMAT index_format,
                        uint32_t index_buffer_offset, uint32_t vertex_srv_count,
                        uint32_t pixel_srv_count, uint32_t vertex_sampler_count,
                        uint32_t pixel_sampler_count);
  // Writes every constant buffer whose update was deferred to the GPU now (the
  // record path holds a draw's updates until replay; this restores them when
  // the draw falls back to the immediate path).
  void FlushDeferredConstantBuffers();
  // Replays the open tape before a diagnostic reads GPU output (a texture or
  // surface dump, a probe readback, the resolve's source check). A no-op when
  // recording is off or the tape is empty.
  bool DrainTapeForDiagnostic(const char* reason);

  // fh1_debug_surface_dump: once, at fh1_debug_surface_dump_swap, write every
  // native EDRAM surface (colour and depth) as a PPM with a stats line and log
  // the tile ownership histogram - which surface holds the frame, by key.
  void DumpNativeSurfacesOnce();
  // fh1_debug_presented_pair: once, at fh1_debug_presented_pair_swap, read the
  // composite the presenting resolve sampled and the untiled swap texture the
  // presenter paints in one call, so both images carry the same swap counter
  // and are provably the same moment rather than two captures at neighbouring
  // times.
  void DumpPresentedPairOnce();
  // The source key the last frame-sized resolve actually sampled (its own key
  // or the owner the tiles named), for the surface dump's frame record.
  Fh1SurfaceKey last_resolve_source_key_;
  uint64_t last_resolve_source_swap_ = ~uint64_t(0);

  D3D11GraphicsSystem* d3d11_graphics_system_;
  uint64_t presented_output_frames_ = 0;
  // Time spent in the swap's wait-for-GPU (the presentation path's CPU/GPU
  // serialisation point), reported at shutdown.
  uint64_t swap_wait_ticks_ = 0;
  uint64_t swap_wait_count_ = 0;
  // IssueSwap logs an unresolvable frontbuffer once instead of every swap.
  bool swap_texture_logged_ = false;
  // Where the last native resolve wrote, and when - the other half of the
  // fh1_swap_fetch_trace record (what the guest asked the swap to present vs
  // where the resolve path actually put the frame).
  uint32_t last_resolve_guest_base_ = 0;
  uint32_t last_resolve_length_ = 0;
  uint32_t last_resolve_pitch_ = 0;
  uint32_t last_resolve_info_ = 0;
  uint64_t last_resolve_swap_ = 0;
  bool has_last_resolve_ = false;
  uint64_t resolve_call_count_ = 0;
  // Every surface identity the draw path has bound as a render target, so the
  // resolve trace can say whether the surface a resolve reads was ever drawn
  // into (the answer separates "empty source" from "wrong surface").
  std::unordered_set<uint32_t> ever_bound_render_target_keys_;
  uint32_t render_target_key_logs_ = 0;
  // Frontbuffer readback (GPU mirror -> guest memory) scratch and tallies:
  // fh1_d3d11_resolve_readback lands the swap fetch's extent once per swap.
  bool resolve_owner_was_draw_target_ = false;
  std::vector<uint8_t> resolve_readback_scratch_;
  uint64_t resolve_readback_count_ = 0;
  uint64_t resolve_readback_bytes_ = 0;
  bool resolve_readback_failed_logged_ = false;
  // RefreshResolvedGuestRange tallies: ranges the GPU wrote that a CPU-side
  // texture upload asked for, and how much of the mirror that moved.
  uint64_t resolve_refresh_count_ = 0;
  uint64_t resolve_refresh_bytes_ = 0;
  uint64_t resolve_refresh_swap_mark_ = 0;
  uint64_t resolve_refresh_swap_bytes_ = 0;
  bool resolve_refresh_failed_logged_ = false;
  // Refresh wait batching: the GPU wait in RefreshResolvedGuestRange is the
  // refresh path's serialisation point, and a swap's texture loads ask for
  // several GPU-written ranges (base + mips, then the next texture) that the
  // same resolves produced. Once a wait has drained the GPU, a later refresh
  // for a range no newer resolve has written needs no second wait - the
  // mirror data it reads was already final when the last wait returned, and
  // the readback's own Map orders the copy after it. So a refresh skips the
  // wait (and the tape replay, which only matters when a resolve landed)
  // when resolve_call_count_ is unchanged since the last refresh wait.
  // refresh_waited_resolve_count_ is the resolve count at that wait.
  uint64_t refresh_waited_resolve_count_ = 0;
  bool refresh_waited_ = false;
  // Refresh wait cost: time in WaitForGpu for refreshes, and how many waits
  // the batching skipped vs performed. Logged with the periodic refresh line.
  uint64_t refresh_wait_ticks_ = 0;
  uint64_t refresh_waits_done_ = 0;
  uint64_t refresh_waits_skipped_ = 0;
  // fh1_debug_surface_dump fires once per session.
  bool surface_dump_done_ = false;
  // fh1_debug_presented_pair dumps this many consecutive swaps from its start.
  uint32_t presented_pair_dumps_ = 0;
  Microsoft::WRL::ComPtr<ID3D11Query> gpu_wait_query_;
  DrawCounters draw_counters_;

  // Host render target configuration (Phase 4) and the FH1 native EDRAM
  // surface layer + SM5.0 resolve path it feeds. The FH1 route owns its
  // render targets (EDRAM surface keys -> native ID3D11 textures).
  std::unique_ptr<D3D11HostRenderConfig> host_render_config_;
  std::unique_ptr<D3D11Fh1NativeSurfaces> native_surfaces_;
  std::unique_ptr<D3D11Fh1NativeResolve> native_resolve_;
  // fh1_native_executor: derives the draw's EDRAM surfaces, claims their tiles
  // and binds them through the executor instead of the render target path.
  // Null when the cvar is off, which keeps the previous path exactly.
  std::unique_ptr<D3D11Fh1NativeExecutor> native_executor_;

  std::unique_ptr<D3D11SharedMemory> shared_memory_;
  std::unique_ptr<D3D11PrimitiveProcessor> primitive_processor_;
  std::unique_ptr<D3D11TextureCache> texture_cache_;
  std::unique_ptr<D3D11RenderTargetCache> render_target_cache_;
  std::unique_ptr<D3D11PipelineCache> pipeline_cache_;

  // Phase 3 M2: the command tape, its state map and the immediate replay sink
  // are constructed only when fh1_d3d11_command_lists is on. Off, none of the
  // recording state exists and every pass runs the immediate path unchanged.
  bool command_lists_enabled_ = false;
  std::unique_ptr<D3D11CommandList> command_list_;
  std::unique_ptr<D3D11CommandSink> command_sink_;
  std::unique_ptr<D3D11ResourceStateTracker> resource_state_tracker_;
  std::unique_ptr<D3D11SubmissionQueue> submission_queue_;
  // Tape accounting for the phase measurements (packets/arena per submission).
  uint64_t tape_submit_count_ = 0;
  uint64_t tape_packets_submitted_ = 0;
  uint64_t tape_arena_bytes_submitted_ = 0;
  uint64_t tape_resolve_packets_ = 0;
  uint64_t tape_draw_packets_ = 0;
  // A draw recorded, submitted at the arena/packet cap, and retried - the
  // overflow safety valve, not the normal path.
  uint64_t tape_draw_fallbacks_ = 0;
  // A draw the packet cannot hold (more texture slots or samplers than the
  // packet binds, an indexed draw with no index buffer) - executed immediately.
  uint64_t tape_draw_unrecordable_ = 0;
  // A recorded draw whose viewport or scissor was empty at record time: the
  // immediate path had a default (full-target) viewport, so replaying a zero-
  // sized one would drop its geometry. Counted rather than trusted.
  uint64_t tape_draw_empty_viewport_ = 0;
  uint64_t tape_texture_upload_packets_ = 0;
  bool recording_draw_ = false;
  bool recording_texture_uploads_ = false;

  // System shader constants.
  DxbcShaderTranslator::SystemConstants system_constants_;
  // Float constant usage masks of the last draw call.
  uint64_t current_float_constant_map_vertex_[4] = {};
  uint64_t current_float_constant_map_pixel_[4] = {};
  // b0 system, b1 float (per stage), b2 bool/loop, b3 fetch constants.
  ConstantBufferBinding cbuffer_binding_system_;
  ConstantBufferBinding cbuffer_binding_float_vertex_;
  ConstantBufferBinding cbuffer_binding_float_pixel_;
  ConstantBufferBinding cbuffer_binding_bool_loop_;
  ConstantBufferBinding cbuffer_binding_fetch_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> dummy_constant_buffer_;

  // Sampler state cache (desc -> state), command processor thread only.
  std::vector<std::pair<uint64_t, Microsoft::WRL::ComPtr<ID3D11SamplerState>>> sampler_cache_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> GetSamplerState(
      const D3D11_SAMPLER_DESC& desc, uint64_t desc_hash);

  // The currently bound per-draw bindings (to skip redundant Sets).
  ID3D11Buffer* current_constant_buffers_[5] = {};
  ID3D11ShaderResourceView* current_srvs_[1 + DxbcShaderTranslator::kMaxTextureBindings] = {};
  uint32_t current_srv_count_ = 0;
  ID3D11SamplerState* current_samplers_[DxbcShaderTranslator::kMaxSamplerBindings] = {};
  uint32_t current_sampler_count_ = 0;

  // Vertex buffer residency (in-shader fetch reads the shared-memory mirror,
  // so this is residency only, no binding).
  struct VertexBufferState {
    uint32_t address = UINT32_MAX;
    uint32_t size = UINT32_MAX;
  };
  static constexpr uint32_t kVertexBufferCount = 96;
  std::array<VertexBufferState, kVertexBufferCount> vertex_buffer_states_{};
  uint64_t vertex_buffers_in_sync_[2] = {};

  // Fixed-function dynamic state tracking.
  D3D11_VIEWPORT ff_viewport_ = {};
  D3D11_RECT ff_scissor_ = {};
  float ff_blend_factor_[4] = {};
  uint32_t ff_stencil_ref_ = 0;
  bool ff_viewport_update_needed_ = true;
  bool ff_scissor_update_needed_ = true;
  bool ff_blend_factor_update_needed_ = true;
  bool ff_stencil_ref_update_needed_ = true;
};

}  // namespace rex::graphics::d3d11
