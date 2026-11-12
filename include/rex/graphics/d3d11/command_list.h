/**
 * @file        graphics/d3d11/command_list.h
 * @brief       Deferred command tape for the D3D11 backend (Phase 3 M1)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

namespace rex::graphics::d3d11 {

// The subset of resource states the command-list path records. The values are
// bits so read-only combinations (the guest-memory mirror's shader-resource,
// index-buffer and vertex-buffer uses) can be expressed as one state.
enum class D3D11ResourceState : uint32_t {
  kCommon = 0,
  kRenderTarget = 1u << 0,
  kDepthWrite = 1u << 1,
  kShaderResource = 1u << 2,
  kUnorderedAccess = 1u << 3,
  kCopySource = 1u << 4,
  kCopyDest = 1u << 5,
  kPresent = 1u << 6,
  kResolveSource = 1u << 7,
  kResolveDest = 1u << 8,
  kIndexBuffer = 1u << 9,
  kVertexBuffer = 1u << 10,
};

constexpr D3D11ResourceState operator|(D3D11ResourceState a, D3D11ResourceState b) {
  return D3D11ResourceState(uint32_t(a) | uint32_t(b));
}

constexpr D3D11ResourceState operator&(D3D11ResourceState a, D3D11ResourceState b) {
  return D3D11ResourceState(uint32_t(a) & uint32_t(b));
}

constexpr bool HasState(D3D11ResourceState state, D3D11ResourceState required) {
  return required == D3D11ResourceState::kCommon
             ? state == D3D11ResourceState::kCommon
             : (uint32_t(state) & uint32_t(required)) == uint32_t(required);
}

// The guest-memory mirror is read by shaders, index fetches and vertex fetch
// in the same pass; this is the combination recorded for it.
constexpr D3D11ResourceState kMirrorReadState = D3D11ResourceState::kShaderResource |
                                                D3D11ResourceState::kIndexBuffer |
                                                D3D11ResourceState::kVertexBuffer;

// A slice of the tape's byte arena. Offsets are from the tape base, not from
// the packet; size is zero for an unused span.
struct D3D11ArenaSpan {
  uint32_t offset = 0;
  uint32_t size = 0;

  bool empty() const { return size == 0; }
};

struct D3D11BarrierPacket {
  Microsoft::WRL::ComPtr<IUnknown> resource;
  D3D11ResourceState before = D3D11ResourceState::kCommon;
  D3D11ResourceState after = D3D11ResourceState::kCommon;
};

struct D3D11UavBarrierPacket {
  Microsoft::WRL::ComPtr<IUnknown> resource;
};

struct D3D11UnbindRenderTargetsPacket {};

// Phase 3 M2: unbinds the guest-memory mirror's SRV from the graphics stages'
// slot 0 before a compute pass binds a view of the same buffer. Direct3D 11
// drops a view bind silently when the resource is already bound as an
// incompatible view of itself (the resolve path's blanking defect), so the
// recorded stream carries the unbind explicitly rather than relying on the
// last draw's state.
struct D3D11UnbindGraphicsSrvPacket {};

struct D3D11ConstantBufferBinding {
  Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
  D3D11ArenaSpan data;
};

struct D3D11DispatchPacket {
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
  std::array<D3D11ConstantBufferBinding, 5> constant_buffers;
  std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 2> srvs;
  uint32_t srv_count = 0;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
  uint32_t group_count_x = 0;
  uint32_t group_count_y = 0;
  uint32_t group_count_z = 1;
  bool unbind_om = false;
};

struct D3D11CopyBufferPacket {
  Microsoft::WRL::ComPtr<ID3D11Buffer> source;
  Microsoft::WRL::ComPtr<ID3D11Buffer> destination;
  uint32_t source_offset = 0;
  uint32_t destination_offset = 0;
  uint32_t length = 0;
};

struct D3D11CopyResourcePacket {
  Microsoft::WRL::ComPtr<ID3D11Resource> source;
  Microsoft::WRL::ComPtr<ID3D11Resource> destination;
};

struct D3D11CopySubresourcePacket {
  Microsoft::WRL::ComPtr<ID3D11Resource> source;
  Microsoft::WRL::ComPtr<ID3D11Resource> destination;
  uint32_t source_subresource = 0;
  uint32_t destination_subresource = 0;
  uint32_t destination_x = 0;
  uint32_t destination_y = 0;
  uint32_t destination_z = 0;
  D3D11_BOX box = {};
};

struct D3D11UploadBufferPacket {
  Microsoft::WRL::ComPtr<ID3D11Buffer> destination;
  uint32_t destination_offset = 0;
  D3D11ArenaSpan data;
};

struct D3D11UploadTexturePacket {
  Microsoft::WRL::ComPtr<ID3D11Resource> destination;
  uint32_t destination_subresource = 0;
  D3D11_BOX box = {};
  D3D11ArenaSpan data;
  uint32_t row_pitch = 0;
  uint32_t depth_pitch = 0;
};

struct D3D11ClearRenderTargetPacket {
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target;
  std::array<float, 4> color = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct D3D11ClearDepthStencilPacket {
  Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth_stencil;
  uint32_t flags = 0;
  float depth = 1.0f;
  uint8_t stencil = 0;
};

struct D3D11ResolveSubresourcePacket {
  Microsoft::WRL::ComPtr<ID3D11Resource> source;
  Microsoft::WRL::ComPtr<ID3D11Resource> destination;
  uint32_t source_subresource = 0;
  uint32_t destination_subresource = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

// Phase 3 M3: the per-stage texture slots a draw can bind. One slot is the
// guest-memory mirror (t0, in-shader fetch); the rest are the shader's fetch
// constants. 33 covers every FH1 fetch constant with room to spare, and a draw
// that binds more is executed immediately instead of recorded (logged).
static constexpr uint32_t kMaxDrawTextureSrvs = 33;
// Feature level 11_0 binds 16 samplers per stage at most.
static constexpr uint32_t kMaxDrawSamplers = 16;
// The render targets the draw path binds (one per colour slot plus depth).
static constexpr uint32_t kMaxDrawRenderTargets = 8;

// One of the five constant buffers a draw binds, with the exact bytes the
// record-time update held (the packet is self-contained: replay does not
// depend on the buffer's contents between submissions).
struct D3D11DrawConstantBuffer {
  Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
  D3D11ArenaSpan data;
};

// Phase 3 M3: a complete draw. Shaders, state objects, constant blobs, views,
// samplers, targets, viewport/scissor, topology, the index buffer and the draw
// arguments are captured at record time; replay rebinds all of it and issues
// the one Draw or DrawIndexed. Capturing from the context (rather than
// describing the intended state a second time) keeps the packet a record of
// what was actually bound, including state the draw path sets through cached
// null-state calls.
struct D3D11DrawPacket {
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader;
  Microsoft::WRL::ComPtr<ID3D11GeometryShader> geometry_shader;
  Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_stencil_state;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_state;
  // b0 system, b1 float (VS and PS), b2 bool/loop, b3 fetch.
  std::array<D3D11DrawConstantBuffer, 5> constant_buffers;
  std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, 4> vs_constant_buffers;
  std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, 4> ps_constant_buffers;
  std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, kMaxDrawTextureSrvs> vs_srvs;
  uint32_t vs_srv_count = 0;
  std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, kMaxDrawTextureSrvs> ps_srvs;
  uint32_t ps_srv_count = 0;
  std::array<Microsoft::WRL::ComPtr<ID3D11SamplerState>, kMaxDrawSamplers> vs_samplers;
  uint32_t vs_sampler_count = 0;
  std::array<Microsoft::WRL::ComPtr<ID3D11SamplerState>, kMaxDrawSamplers> ps_samplers;
  uint32_t ps_sampler_count = 0;
  std::array<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>, kMaxDrawRenderTargets>
      render_targets;
  uint32_t render_target_count = 0;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth_stencil_view;
  D3D11_VIEWPORT viewport = {};
  D3D11_RECT scissor = {};
  std::array<float, 4> blend_factor = {0.0f, 0.0f, 0.0f, 0.0f};
  uint32_t sample_mask = UINT32_MAX;
  uint32_t stencil_ref = 0;
  D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  Microsoft::WRL::ComPtr<ID3D11Buffer> index_buffer;
  DXGI_FORMAT index_format = DXGI_FORMAT_UNKNOWN;
  uint32_t index_buffer_offset = 0;
  uint32_t vertex_count = 0;
  bool indexed = false;
};

using D3D11Command =
    std::variant<D3D11BarrierPacket, D3D11UavBarrierPacket, D3D11UnbindRenderTargetsPacket,
                 D3D11UnbindGraphicsSrvPacket, D3D11DispatchPacket, D3D11CopyBufferPacket,
                 D3D11CopyResourcePacket,
                 D3D11CopySubresourcePacket, D3D11UploadBufferPacket, D3D11UploadTexturePacket,
                 D3D11ClearRenderTargetPacket, D3D11ClearDepthStencilPacket,
                 D3D11ResolveSubresourcePacket, D3D11DrawPacket>;

class D3D11CommandList;

// Replays one recorded command. The D3D11 implementation forwards to the
// immediate context; tests record the call without a device. The tape is
// passed alongside the packet so arena-backed payloads (constant buffers,
// uploads) can be resolved with D3D11CommandList::Data.
class D3D11CommandSink {
 public:
  virtual ~D3D11CommandSink() = default;

  virtual void Execute(const D3D11Command& command, const D3D11CommandList& tape) = 0;
};

// Creates the sink that replays a tape onto the immediate context. Defined in
// command_list_immediate_sink.cpp, which is not linked into the device-free
// unit tests.
std::unique_ptr<D3D11CommandSink> CreateD3D11ImmediateCommandSink(ID3D11DeviceContext* context);

// Records the D3D11 backend's frame and swap passes for replay on the single
// immediate context. Phase 3 M2 records the resolve and transfer path: the
// mirror upload (UploadBuffer), the dispatch and the staging-to-mirror copy
// (CopyBuffer). Phase 3 M3 adds the draw path (DrawPacket) and the texture
// cache's uploads (UploadTexture). The swap path is still immediate.
class D3D11CommandList {
 public:
  static constexpr uint32_t kMaxPackets = 32768;
  static constexpr size_t kMaxArenaBytes = 64u << 20;
  static constexpr size_t kArenaAlignment = 16;

  // The caps exist for focused tests as well as the production tape; the
  // defaults are the limits the backend records under.
  D3D11CommandList(size_t max_packets = kMaxPackets, size_t max_arena_bytes = kMaxArenaBytes);
  ~D3D11CommandList();

  D3D11CommandList(const D3D11CommandList&) = delete;
  D3D11CommandList& operator=(const D3D11CommandList&) = delete;

  // Appends a default-constructed packet and returns it for field assignment.
  // Returns null when the packet or arena cap has been reached; the caller
  // must treat that as an overflow and stop recording.
  template <typename T>
  T* Append() {
    if (overflow_ || commands_.size() >= max_packets_) {
      overflow_ = true;
      return nullptr;
    }
    commands_.emplace_back(T{});
    return std::get_if<T>(&commands_.back());
  }

  // Copies bytes into the arena and returns their span. Returns false and sets
  // overflow when the arena cap would be exceeded; the output span is cleared.
  bool AppendBytes(const void* data, size_t size, D3D11ArenaSpan& span_out);

  // Appends an UploadBuffer packet with its bytes copied into the arena (the
  // mirror-upload recording path, Phase 2 H42). Returns false and sets overflow
  // when either cap is reached; no packet is left on the tape on failure.
  bool AppendUploadBufferPacket(ID3D11Buffer* destination, uint32_t destination_offset,
                                const void* data, uint32_t size);

  // Drops every packet appended after `size` (a command_count from before a
  // batch builder ran). Releases the dropped packets' COM references; arena
  // bytes are not reclaimed. Overflow stays sticky until Reset.
  void Truncate(size_t size);

  // Replays every packet in order. The sink must outlive the call.
  void Replay(D3D11CommandSink& sink) const;

  // Drops every packet and releases every COM reference the packets hold. The
  // arena and packet buffers keep their capacity for the next frame.
  void Reset();

  bool empty() const { return commands_.empty(); }
  size_t command_count() const { return commands_.size(); }
  size_t arena_bytes() const { return arena_.size(); }
  bool overflowed() const { return overflow_; }

  const std::vector<D3D11Command>& commands() const { return commands_; }

  // Null when the span is empty or outside the arena.
  const uint8_t* Data(D3D11ArenaSpan span) const;

 private:
  std::vector<D3D11Command> commands_;
  std::vector<uint8_t> arena_;
  size_t max_packets_ = kMaxPackets;
  size_t max_arena_bytes_ = kMaxArenaBytes;
  bool overflow_ = false;
};

}  // namespace rex::graphics::d3d11
