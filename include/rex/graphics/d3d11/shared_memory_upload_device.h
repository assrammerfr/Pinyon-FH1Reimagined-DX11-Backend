/**
 * @file        graphics/d3d11/shared_memory_upload_device.h
 * @brief       D3D11 backend: device-side shared memory upload sink (Phase 2
 *              foundation)
 *
 *              Owns the host GPU mirror of the guest physical shared memory:
 *              one 512 MB DEFAULT buffer that later phases bind as shader
 *              resource / unordered access views for texture untiling and
 *              vertex fetch, plus one fixed staging buffer the upload path
 *              copies guest pages through.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <vector>

#include <rex/graphics/d3d11/shared_memory_upload.h>
#include <rex/ui/d3d11/d3d11_api.h>

namespace rex::graphics::d3d11 {

class D3D11CommandList;

class D3D11SharedMemoryDeviceUploadSink : public D3D11SharedMemoryUploadSink {
 public:
  D3D11SharedMemoryDeviceUploadSink(ID3D11Device* device,
                                    ID3D11DeviceContext* immediate_context);
  ~D3D11SharedMemoryDeviceUploadSink() override;

  bool CreateHostBuffer(uint32_t size_bytes) override;
  bool UploadToHostBuffer(uint32_t offset_bytes, const void* source,
                          uint32_t length_bytes) override;
  void DestroyHostBuffer() override;

  // Phase 3 M2: while a tape is set, UploadToHostBuffer appends UploadBuffer
  // packets (bytes copied into the tape arena) instead of staging through the
  // immediate context, so a mirror upload stays ordered with the recorded
  // resolve dispatches (Phase 2 H42). The caller sets this around the resolve
  // path's RequestRange and clears it when leaving the recording scope; a tape
  // overflow falls back to the immediate upload for that chunk, logged once.
  void SetUploadRecorder(D3D11CommandList* tape) { upload_recorder_ = tape; }
  void ClearUploadRecorder() { upload_recorder_ = nullptr; }

  // The resolve staging buffer the recorded staging-to-mirror copy packet
  // names as its source (nullptr before the first EnsureResolveStaging).
  ID3D11Buffer* GetResolveStagingBuffer() const { return resolve_staging_; }

  // Copies length_bytes from the mirror at offset_bytes back to destination
  // through a CPU-readable staging buffer - the resolve readback path. The
  // mirror is a DEFAULT buffer written by the resolve's UAV, so its contents
  // reach guest memory only through this. Called on the command processor
  // thread with the GPU idle after the resolve; length may exceed
  // kSharedMemoryUploadChunkBytes (chunked internally).
  bool ReadbackFromHostBuffer(uint32_t offset_bytes, void* destination, uint32_t length_bytes);

  // Diagnostic (fh1_debug_resolve_small_uav): a small dedicated raw buffer and
  // its UAV, used to test whether the resolve's UAV writes land at all when the
  // view is not the 512 MB guest mirror. Created on first use.
  ID3D11UnorderedAccessView* GetProbeBufferUAV();

  // A raw UAV over a WINDOW of the guest mirror, [start_byte, start_byte +
  // length_bytes), for resolve writes: the whole-mirror view (512 MB) is
  // silently dropped by the driver on this device, so writes go through a
  // window instead. start_byte must be 4-byte aligned; length is rounded down
  // to a multiple of 4. Views are cached per (start, length).
  ID3D11UnorderedAccessView* GetHostBufferUAVWindow(uint32_t start_byte, uint32_t length_bytes);

  // The resolve staging buffer: a small raw buffer whose UAV stores land on
  // this device even though the 512 MB mirror's do not above a certain
  // address. Grows to hold at least length_bytes (4-byte aligned), up to
  // kResolveStagingMaxBytes. nullptr if the request exceeds that.
  ID3D11UnorderedAccessView* EnsureResolveStaging(uint32_t length_bytes);
  // Copies length_bytes of the staging buffer at offset 0 into the mirror at
  // host_offset - the copy that lands the resolved extent in guest memory.
  bool CopyResolveStagingToHost(uint32_t host_offset, uint32_t length_bytes);
  // Reads back the resolve staging buffer (probe: do the staging stores land?).
  bool ReadbackResolveStaging(uint32_t offset_bytes, void* destination, uint32_t length_bytes);
  static constexpr uint32_t kResolveStagingMaxBytes = 64u << 20;
  bool ReadbackProbeBuffer(uint32_t offset_bytes, void* destination, uint32_t length_bytes);

  // The mirror, for the texture cache and the primitive processor to create
  // views on (Phase 2 onward).
  ID3D11Buffer* GetHostBuffer() const { return host_buffer_; }

  // The raw byte-addressable SRV of the whole mirror (t0) - in-shader vertex
  // fetch and guest texture reads (Phase 3). Created on demand; nullptr if
  // creation failed (logged).
  ID3D11ShaderResourceView* GetHostBufferSRV();

  // The raw byte-addressable UAV of the whole mirror, for the FH1 native
  // resolve path to write the guest texture word layout into (Phase 4).
  // Created once with the buffer; nullptr if creation failed (logged).
  ID3D11UnorderedAccessView* GetHostBufferUAV();

  // The typed 4x32-bit view of the whole mirror (R32G32B32A32_UINT, element =
  // one guest texture word) for the word-form texture-cache shaders, which
  // index the mirror as an array of dwords rather than byte addresses. Created
  // on demand next to the raw SRV on the same buffer; nullptr if this driver
  // refuses typed and raw views on one resource (logged with the HRESULT).
  ID3D11ShaderResourceView* GetHostBufferTypedSRV();

 private:
  ID3D11Device* device_;
  ID3D11DeviceContext* immediate_context_;

  // The guest physical shared memory mirror.
  ID3D11Buffer* host_buffer_ = nullptr;
  uint32_t host_buffer_size_ = 0;
  ID3D11ShaderResourceView* host_buffer_srv_ = nullptr;
  ID3D11ShaderResourceView* host_buffer_typed_srv_ = nullptr;
  ID3D11UnorderedAccessView* host_buffer_uav_ = nullptr;
  // Fixed-size CPU-side staging the guest pages are copied through.
  ID3D11Buffer* staging_buffer_ = nullptr;
  // CPU-readable staging for resolve readbacks, created on first use.
  ID3D11Buffer* readback_buffer_ = nullptr;
  // Phase 3 M2: the tape the mirror uploads are recorded into, or nullptr for
  // the immediate upload path.
  D3D11CommandList* upload_recorder_ = nullptr;
  // Resolve staging buffer (the production resolve destination).
  ID3D11Buffer* resolve_staging_ = nullptr;
  ID3D11UnorderedAccessView* resolve_staging_uav_ = nullptr;
  uint32_t resolve_staging_size_ = 0;
  // Diagnostic small raw buffer (fh1_debug_resolve_small_uav).
  ID3D11Buffer* probe_buffer_ = nullptr;
  ID3D11UnorderedAccessView* probe_buffer_uav_ = nullptr;
  uint32_t probe_buffer_size_ = 0;
  // Cache of mirror windows for the resolve destination writes.
  struct WindowView {
    uint32_t start = 0;
    uint32_t length = 0;
    ID3D11UnorderedAccessView* view = nullptr;
  };
  std::vector<WindowView> window_views_;
};

}  // namespace rex::graphics::d3d11
