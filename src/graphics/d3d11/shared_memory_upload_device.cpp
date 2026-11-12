/**
 * @file        graphics/d3d11/shared_memory_upload_device.cpp
 * @brief       D3D11 backend: device-side shared memory upload sink (Phase 2
 *              foundation)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/shared_memory_upload_device.h>

#include <atomic>
#include <cstring>
#include <vector>

#include <rex/graphics/d3d11/command_list.h>
#include <rex/logging.h>

namespace rex::graphics::d3d11 {

D3D11SharedMemoryDeviceUploadSink::D3D11SharedMemoryDeviceUploadSink(
    ID3D11Device* device, ID3D11DeviceContext* immediate_context)
    : device_(device), immediate_context_(immediate_context) {}

D3D11SharedMemoryDeviceUploadSink::~D3D11SharedMemoryDeviceUploadSink() {
  DestroyHostBuffer();
}

bool D3D11SharedMemoryDeviceUploadSink::CreateHostBuffer(uint32_t size_bytes) {
  DestroyHostBuffer();

  // Direct3D 11 has no sparse buffers at feature level 11_0, so the full
  // mirror is committed, as in the non-tiled Direct3D 12 shared memory
  // fallback - video memory usage may increase significantly because a full
  // buffer is created.
  D3D11_BUFFER_DESC host_buffer_desc = {};
  host_buffer_desc.ByteWidth = size_bytes;
  host_buffer_desc.Usage = D3D11_USAGE_DEFAULT;
  // Shader resource for in-shader vertex fetch (t0 ByteAddressBuffer) and
  // guest texture reads, unordered access for the resolve path, and index
  // buffer for guest-DMA index draws (Phase 3).
  host_buffer_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS |
                               D3D11_BIND_INDEX_BUFFER;
  host_buffer_desc.CPUAccessFlags = 0;
  // Raw views (RWByteAddressBuffer) for the FH1 native resolve path.
  host_buffer_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  host_buffer_desc.StructureByteStride = 0;
  if (FAILED(device_->CreateBuffer(&host_buffer_desc, nullptr, &host_buffer_))) {
    REXGPU_ERROR("D3D11 shared memory: failed to create the {} MB host buffer",
                 size_bytes >> 20);
    return false;
  }
  host_buffer_size_ = size_bytes;

  D3D11_BUFFER_DESC staging_buffer_desc = {};
  staging_buffer_desc.ByteWidth = kSharedMemoryUploadChunkBytes;
  staging_buffer_desc.Usage = D3D11_USAGE_STAGING;
  staging_buffer_desc.BindFlags = 0;
  staging_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  staging_buffer_desc.MiscFlags = 0;
  staging_buffer_desc.StructureByteStride = 0;
  if (FAILED(
          device_->CreateBuffer(&staging_buffer_desc, nullptr, &staging_buffer_))) {
    REXGPU_ERROR("D3D11 shared memory: failed to create the staging buffer");
    DestroyHostBuffer();
    return false;
  }

  REXGPU_INFO(
      "D3D11 shared memory: created the {} MB host mirror (no tiled resources "
      "at feature level 11_0)",
      size_bytes >> 20);
  // Prove at startup that this driver hosts the typed word view next to the
  // raw ones (the word-form texture-cache shaders need it); the result is
  // logged with its HRESULT, and the view is reused on demand afterwards.
  GetHostBufferTypedSRV();
  return true;
}

bool D3D11SharedMemoryDeviceUploadSink::UploadToHostBuffer(
    uint32_t offset_bytes, const void* source, uint32_t length_bytes) {
  if (host_buffer_ == nullptr || staging_buffer_ == nullptr) {
    REXGPU_ERROR("D3D11 shared memory: upload without a host buffer");
    return false;
  }

  const uint8_t* source_bytes = static_cast<const uint8_t*>(source);
  while (length_bytes != 0) {
    uint32_t chunk_bytes =
        length_bytes < kSharedMemoryUploadChunkBytes ? length_bytes
                                                     : kSharedMemoryUploadChunkBytes;
    // Phase 3 M2: the recorded form. The bytes are copied into the tape arena
    // now and written to the mirror when the tape is submitted, in order with
    // the recorded resolve dispatches (Phase 2 H42).
    if (upload_recorder_) {
      if (upload_recorder_->AppendUploadBufferPacket(host_buffer_, offset_bytes, source_bytes,
                                                     chunk_bytes)) {
        source_bytes += chunk_bytes;
        offset_bytes += chunk_bytes;
        length_bytes -= chunk_bytes;
        continue;
      }
      static std::atomic<bool> overflow_logged{false};
      if (!overflow_logged.exchange(true)) {
        REXGPU_WARN(
            "D3D11 shared memory: the command tape is full; mirror uploads fall back to the "
            "immediate path from {:08X}+{} (the tape must not overflow in a normal run)",
            offset_bytes, length_bytes);
      }
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(immediate_context_->Map(staging_buffer_, 0, D3D11_MAP_WRITE, 0,
                                       &mapped))) {
      REXGPU_ERROR(
          "D3D11 shared memory: failed to map the staging buffer for {:08X}+{}",
          offset_bytes, length_bytes);
      return false;
    }
    std::memcpy(mapped.pData, source_bytes, chunk_bytes);
    immediate_context_->Unmap(staging_buffer_, 0);

    D3D11_BOX staging_box = {};
    staging_box.left = 0;
    staging_box.top = 0;
    staging_box.front = 0;
    staging_box.right = chunk_bytes;
    staging_box.bottom = 1;
    staging_box.back = 1;
    immediate_context_->CopySubresourceRegion(
        host_buffer_, 0, offset_bytes, 0, 0, staging_buffer_, 0, &staging_box);

    source_bytes += chunk_bytes;
    offset_bytes += chunk_bytes;
    length_bytes -= chunk_bytes;
  }
  return true;
}

bool D3D11SharedMemoryDeviceUploadSink::ReadbackFromHostBuffer(uint32_t offset_bytes,
                                                             void* destination,
                                                             uint32_t length_bytes) {
  if (host_buffer_ == nullptr || destination == nullptr) {
    REXGPU_ERROR("D3D11 shared memory: readback without a host buffer");
    return false;
  }
  if (!readback_buffer_) {
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = kSharedMemoryUploadChunkBytes;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device_->CreateBuffer(&desc, nullptr, &readback_buffer_))) {
      readback_buffer_ = nullptr;
      REXGPU_ERROR("D3D11 shared memory: failed to create the readback staging buffer");
      return false;
    }
  }

  uint8_t* destination_bytes = static_cast<uint8_t*>(destination);
  while (length_bytes != 0) {
    const uint32_t chunk_bytes =
        length_bytes < kSharedMemoryUploadChunkBytes ? length_bytes : kSharedMemoryUploadChunkBytes;
    D3D11_BOX source_box = {};
    source_box.left = offset_bytes;
    source_box.top = 0;
    source_box.front = 0;
    source_box.right = offset_bytes + chunk_bytes;
    source_box.bottom = 1;
    source_box.back = 1;
    immediate_context_->CopySubresourceRegion(readback_buffer_, 0, 0, 0, 0, host_buffer_, 0,
                                              &source_box);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(immediate_context_->Map(readback_buffer_, 0, D3D11_MAP_READ, 0, &mapped))) {
      REXGPU_ERROR("D3D11 shared memory: failed to map the readback staging buffer for {:08X}+{}",
                   offset_bytes, chunk_bytes);
      return false;
    }
    std::memcpy(destination_bytes, mapped.pData, chunk_bytes);
    immediate_context_->Unmap(readback_buffer_, 0);

    destination_bytes += chunk_bytes;
    offset_bytes += chunk_bytes;
    length_bytes -= chunk_bytes;
  }
  return true;
}

ID3D11UnorderedAccessView* D3D11SharedMemoryDeviceUploadSink::EnsureResolveStaging(
    uint32_t length_bytes) {
  length_bytes = (length_bytes + 3u) & ~3u;
  if (!length_bytes || length_bytes > kResolveStagingMaxBytes) {
    return nullptr;
  }
  if (resolve_staging_uav_ && resolve_staging_size_ >= length_bytes) {
    return resolve_staging_uav_;
  }
  // Grow in powers of two from 1 MB so a session allocates once or twice.
  uint32_t size = 1u << 20;
  while (size < length_bytes) {
    size <<= 1;
  }
  if (size > kResolveStagingMaxBytes) {
    size = kResolveStagingMaxBytes;
  }
  ID3D11Buffer* buffer = nullptr;
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = size;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  if (FAILED(device_->CreateBuffer(&desc, nullptr, &buffer))) {
    REXGPU_ERROR("D3D11 shared memory: failed to create the {} MB resolve staging buffer",
                 size >> 20);
    return nullptr;
  }
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
  uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uav_desc.Buffer.FirstElement = 0;
  uav_desc.Buffer.NumElements = size / 4;
  uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  ID3D11UnorderedAccessView* uav = nullptr;
  if (FAILED(device_->CreateUnorderedAccessView(buffer, &uav_desc, &uav))) {
    REXGPU_ERROR("D3D11 shared memory: failed to create the {} MB resolve staging UAV",
                 size >> 20);
    buffer->Release();
    return nullptr;
  }
  if (resolve_staging_uav_) {
    resolve_staging_uav_->Release();
  }
  if (resolve_staging_) {
    resolve_staging_->Release();
  }
  resolve_staging_ = buffer;
  resolve_staging_uav_ = uav;
  resolve_staging_size_ = size;
  REXGPU_INFO("D3D11 shared memory: resolve staging buffer ready ({} MB)", size >> 20);
  return resolve_staging_uav_;
}

bool D3D11SharedMemoryDeviceUploadSink::CopyResolveStagingToHost(uint32_t host_offset,
                                                                uint32_t length_bytes) {
  if (!resolve_staging_ || !host_buffer_ || !length_bytes ||
      length_bytes > resolve_staging_size_ || host_offset >= host_buffer_size_ ||
      length_bytes > host_buffer_size_ - host_offset) {
    return false;
  }
  D3D11_BOX box = {};
  box.left = 0;
  box.right = length_bytes;
  box.bottom = 1;
  box.back = 1;
  immediate_context_->CopySubresourceRegion(host_buffer_, 0, host_offset, 0, 0, resolve_staging_, 0,
                                            &box);
  return true;
}

bool D3D11SharedMemoryDeviceUploadSink::ReadbackResolveStaging(uint32_t offset_bytes,
                                                              void* destination,
                                                              uint32_t length_bytes) {
  if (!resolve_staging_ || !length_bytes || offset_bytes >= resolve_staging_size_ ||
      length_bytes > resolve_staging_size_ - offset_bytes) {
    return false;
  }
  if (readback_buffer_ == nullptr) {
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = kSharedMemoryUploadChunkBytes;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device_->CreateBuffer(&desc, nullptr, &readback_buffer_))) {
      readback_buffer_ = nullptr;
      return false;
    }
  }
  uint8_t* destination_bytes = static_cast<uint8_t*>(destination);
  while (length_bytes != 0) {
    const uint32_t chunk_bytes =
        length_bytes < kSharedMemoryUploadChunkBytes ? length_bytes : kSharedMemoryUploadChunkBytes;
    D3D11_BOX box = {};
    box.left = offset_bytes;
    box.right = offset_bytes + chunk_bytes;
    box.bottom = 1;
    box.back = 1;
    immediate_context_->CopySubresourceRegion(readback_buffer_, 0, 0, 0, 0, resolve_staging_, 0,
                                              &box);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(immediate_context_->Map(readback_buffer_, 0, D3D11_MAP_READ, 0, &mapped))) {
      return false;
    }
    std::memcpy(destination_bytes, mapped.pData, chunk_bytes);
    immediate_context_->Unmap(readback_buffer_, 0);
    destination_bytes += chunk_bytes;
    offset_bytes += chunk_bytes;
    length_bytes -= chunk_bytes;
  }
  return true;
}

ID3D11UnorderedAccessView* D3D11SharedMemoryDeviceUploadSink::GetHostBufferUAVWindow(
    uint32_t start_byte, uint32_t length_bytes) {
  if (host_buffer_ == nullptr) {
    return nullptr;
  }
  start_byte &= ~3u;
  length_bytes &= ~3u;
  if (!length_bytes || start_byte >= host_buffer_size_ ||
      length_bytes > host_buffer_size_ - start_byte) {
    return nullptr;
  }
  for (const WindowView& window : window_views_) {
    if (window.start == start_byte && window.length == length_bytes) {
      return window.view;
    }
  }
  D3D11_UNORDERED_ACCESS_VIEW_DESC desc = {};
  desc.Format = DXGI_FORMAT_R32_TYPELESS;
  desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  desc.Buffer.FirstElement = start_byte / 4;
  desc.Buffer.NumElements = length_bytes / 4;
  desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  ID3D11UnorderedAccessView* view = nullptr;
  const HRESULT hr = device_->CreateUnorderedAccessView(host_buffer_, &desc, &view);
  if (FAILED(hr)) {
    REXGPU_ERROR(
        "D3D11 shared memory: failed to create the mirror UAV window {:08X}+{} (hr={:08X})",
        start_byte, length_bytes, uint32_t(hr));
    return nullptr;
  }
  if (window_views_.size() >= 64) {
    // The resolve destinations of one session are few; bound the cache.
    for (WindowView& window : window_views_) {
      if (window.view != nullptr) {
        window.view->Release();
      }
    }
    window_views_.clear();
  }
  window_views_.push_back(WindowView{start_byte, length_bytes, view});
  return view;
}

ID3D11UnorderedAccessView* D3D11SharedMemoryDeviceUploadSink::GetProbeBufferUAV() {
  if (probe_buffer_uav_) {
    return probe_buffer_uav_;
  }
  constexpr uint32_t kProbeBufferBytes = 4u << 20;
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = kProbeBufferBytes;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  if (FAILED(device_->CreateBuffer(&desc, nullptr, &probe_buffer_))) {
    probe_buffer_ = nullptr;
    REXGPU_ERROR("D3D11 shared memory: failed to create the {} MB probe buffer",
                 kProbeBufferBytes >> 20);
    return nullptr;
  }
  probe_buffer_size_ = kProbeBufferBytes;
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
  uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uav_desc.Buffer.FirstElement = 0;
  uav_desc.Buffer.NumElements = kProbeBufferBytes / 4;
  uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  const HRESULT hr = device_->CreateUnorderedAccessView(probe_buffer_, &uav_desc, &probe_buffer_uav_);
  if (FAILED(hr)) {
    probe_buffer_uav_ = nullptr;
    REXGPU_ERROR("D3D11 shared memory: failed to create the probe buffer UAV (hr={:08X})",
                 uint32_t(hr));
    return nullptr;
  }
  REXGPU_INFO("D3D11 shared memory: probe buffer ready ({} MB raw UAV, {} elements)",
              kProbeBufferBytes >> 20, kProbeBufferBytes / 4);
  return probe_buffer_uav_;
}

bool D3D11SharedMemoryDeviceUploadSink::ReadbackProbeBuffer(uint32_t offset_bytes, void* destination,
                                                          uint32_t length_bytes) {
  if (!probe_buffer_ || offset_bytes + length_bytes > probe_buffer_size_) {
    return false;
  }
  if (!readback_buffer_) {
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = kSharedMemoryUploadChunkBytes;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device_->CreateBuffer(&desc, nullptr, &readback_buffer_))) {
      readback_buffer_ = nullptr;
      return false;
    }
  }
  D3D11_BOX box = {};
  box.left = offset_bytes;
  box.right = offset_bytes + length_bytes;
  box.bottom = 1;
  box.back = 1;
  immediate_context_->CopySubresourceRegion(readback_buffer_, 0, 0, 0, 0, probe_buffer_, 0, &box);
  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(immediate_context_->Map(readback_buffer_, 0, D3D11_MAP_READ, 0, &mapped))) {
    return false;
  }
  std::memcpy(destination, mapped.pData, length_bytes);
  immediate_context_->Unmap(readback_buffer_, 0);
  return true;
}

ID3D11ShaderResourceView* D3D11SharedMemoryDeviceUploadSink::GetHostBufferSRV() {
  if (host_buffer_srv_ != nullptr) {
    return host_buffer_srv_;
  }
  if (host_buffer_ == nullptr) {
    return nullptr;
  }
  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  srv_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  // Raw views are BUFFEREX with the RAW flag; a plain BUFFER view of a
  // R32_TYPELESS buffer is rejected with E_INVALIDARG (0x80070057), which
  // silently disabled in-shader shared-memory fetches.
  srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  srv_desc.BufferEx.FirstElement = 0;
  srv_desc.BufferEx.NumElements = host_buffer_size_ / 4;
  srv_desc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
  const HRESULT srv_hr =
      device_->CreateShaderResourceView(host_buffer_, &srv_desc, &host_buffer_srv_);
  if (FAILED(srv_hr)) {
    // Step 1 checkpoint (.musebak6): log the HRESULT like the UAV line below,
    // so an SRV failure reads as E_INVALIDARG (independent desc defect) vs
    // 887A0005 (device already removed).
    REXGPU_ERROR(
        "D3D11 shared memory: failed to create the raw SRV of the {} MB host mirror "
        "(hr={:08X})",
        host_buffer_size_ >> 20, static_cast<uint32_t>(srv_hr));
    host_buffer_srv_ = nullptr;
    return nullptr;
  }
  return host_buffer_srv_;
}

ID3D11ShaderResourceView* D3D11SharedMemoryDeviceUploadSink::GetHostBufferTypedSRV() {
  if (host_buffer_typed_srv_ != nullptr) {
    return host_buffer_typed_srv_;
  }
  if (host_buffer_ == nullptr) {
    return nullptr;
  }
  // One dword of guest memory per element, so the word-form shaders address
  // texels the way the guest texture layout does. The raw SRV stays as-is; the
  // two views coexist on this buffer because it was created with
  // D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS.
  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  srv_desc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
  srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  srv_desc.BufferEx.FirstElement = 0;
  srv_desc.BufferEx.NumElements = host_buffer_size_ / 16;
  const HRESULT srv_hr =
      device_->CreateShaderResourceView(host_buffer_, &srv_desc, &host_buffer_typed_srv_);
  const HRESULT removed_reason =
      FAILED(srv_hr) ? device_->GetDeviceRemovedReason() : S_OK;
  REXGPU_INFO(
      "D3D11 shared memory: typed mirror SRV (R32G32B32A32_UINT, {} elements) {}"
      " hr={:08X} removed_reason={:08X}",
      host_buffer_size_ / 16, FAILED(srv_hr) ? "rejected" : "created",
      static_cast<uint32_t>(srv_hr), static_cast<uint32_t>(removed_reason));
  if (FAILED(srv_hr)) {
    host_buffer_typed_srv_ = nullptr;
    return nullptr;
  }
  return host_buffer_typed_srv_;
}

ID3D11UnorderedAccessView* D3D11SharedMemoryDeviceUploadSink::GetHostBufferUAV() {
  if (host_buffer_uav_ != nullptr) {
    return host_buffer_uav_;
  }
  if (host_buffer_ == nullptr) {
    return nullptr;
  }
  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
  uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
  uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uav_desc.Buffer.FirstElement = 0;
  uav_desc.Buffer.NumElements = host_buffer_size_ / 4;
  uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  const HRESULT uav_hr =
      device_->CreateUnorderedAccessView(host_buffer_, &uav_desc, &host_buffer_uav_);
  if (FAILED(uav_hr)) {
    REXGPU_ERROR(
        "D3D11 shared memory: failed to create the raw UAV of the {} MB host mirror "
        "(hr={:08X})",
        host_buffer_size_ >> 20, static_cast<uint32_t>(uav_hr));
    // One-shot per session: discriminates device loss from a plain creation
    // rejection at the first mirror-UAV failure.
    static std::atomic<bool> device_removed_reason_logged{false};
    if (!device_removed_reason_logged.exchange(true)) {
      const HRESULT removed_reason = device_->GetDeviceRemovedReason();
      REXGPU_ERROR("D3D11 shared memory: first raw UAV failure, GetDeviceRemovedReason={:08X}",
                   static_cast<uint32_t>(removed_reason));
    }
    host_buffer_uav_ = nullptr;
    return nullptr;
  }
  return host_buffer_uav_;
}

void D3D11SharedMemoryDeviceUploadSink::DestroyHostBuffer() {
  if (host_buffer_srv_ != nullptr) {
    host_buffer_srv_->Release();
    host_buffer_srv_ = nullptr;
  }
  if (host_buffer_typed_srv_ != nullptr) {
    host_buffer_typed_srv_->Release();
    host_buffer_typed_srv_ = nullptr;
  }
  if (host_buffer_uav_ != nullptr) {
    host_buffer_uav_->Release();
    host_buffer_uav_ = nullptr;
  }
  if (staging_buffer_ != nullptr) {
    staging_buffer_->Release();
    staging_buffer_ = nullptr;
  }
  if (readback_buffer_ != nullptr) {
    readback_buffer_->Release();
    readback_buffer_ = nullptr;
  }
  if (probe_buffer_uav_ != nullptr) {
    probe_buffer_uav_->Release();
    probe_buffer_uav_ = nullptr;
  }
  if (resolve_staging_uav_ != nullptr) {
    resolve_staging_uav_->Release();
    resolve_staging_uav_ = nullptr;
  }
  if (resolve_staging_ != nullptr) {
    resolve_staging_->Release();
    resolve_staging_ = nullptr;
  }
  resolve_staging_size_ = 0;
  for (WindowView& window : window_views_) {
    if (window.view != nullptr) {
      window.view->Release();
    }
  }
  window_views_.clear();
  if (probe_buffer_ != nullptr) {
    probe_buffer_->Release();
    probe_buffer_ = nullptr;
  }
  probe_buffer_size_ = 0;
  if (host_buffer_ != nullptr) {
    host_buffer_->Release();
    host_buffer_ = nullptr;
  }
  host_buffer_size_ = 0;
}

}  // namespace rex::graphics::d3d11
