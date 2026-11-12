/**
 * @file        graphics/d3d11/shared_memory_upload.h
 * @brief       D3D11 backend: host-side endpoint seam of the shared memory
 *              upload path (Phase 2 foundation)
 *
 *              The upload policy in D3D11SharedMemory (page runs to byte
 *              ranges, bounded chunks, validity before the copy) is device
 *              free; it talks to the host GPU mirror only through this
 *              interface, so the whole path is unit-testable without a
 *              D3D11 device. The device-side implementation lives in
 *              shared_memory_upload_device.h.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

namespace rex::graphics::d3d11 {

// Upper bound of one UploadToHostBuffer call. UploadRanges splits page runs
// into chunks of at most this size in ascending order, so an implementation
// may stage through a single fixed-size buffer.
constexpr uint32_t kSharedMemoryUploadChunkBytes = 1 << 22;

// Host GPU mirror of the guest physical shared memory that D3D11 resources
// (textures, vertex and index buffers) read from. Implementations must be
// usable from the command processor thread only.
class D3D11SharedMemoryUploadSink {
 public:
  virtual ~D3D11SharedMemoryUploadSink() = default;

  // Creates the mirror, size_bytes is SharedMemory::kBufferSize.
  virtual bool CreateHostBuffer(uint32_t size_bytes) = 0;

  // Copies length_bytes from source to offset_bytes in the mirror. Called
  // with a page-aligned offset and length (guest system page granularity),
  // ascending and non-overlapping within one upload request, each call at
  // most kSharedMemoryUploadChunkBytes. The range is already marked valid by
  // the shared memory when this runs, so a failure must be reported: the
  // caller then drops the validity again and retries the range later.
  virtual bool UploadToHostBuffer(uint32_t offset_bytes, const void* source,
                                  uint32_t length_bytes) = 0;

  // Releases the mirror. Safe to call after a failed CreateHostBuffer.
  virtual void DestroyHostBuffer() = 0;
};

}  // namespace rex::graphics::d3d11
