/**
 * @file        graphics/d3d11/shared_memory.cpp
 * @brief       D3D11 backend: shared memory page uploads (Phase 2 foundation)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/shared_memory.h>

#include <algorithm>
#include <utility>

#include <rex/assert.h>
#include <rex/logging.h>

namespace rex::graphics::d3d11 {

D3D11SharedMemory::D3D11SharedMemory(
    memory::Memory& memory,
    std::unique_ptr<D3D11SharedMemoryUploadSink> upload_sink)
    : SharedMemory(memory), upload_sink_(std::move(upload_sink)) {
  assert_true(upload_sink_ != nullptr);
}

D3D11SharedMemory::~D3D11SharedMemory() {
  Shutdown(true);
}

bool D3D11SharedMemory::Initialize() {
  InitializeCommon();
  if (!upload_sink_->CreateHostBuffer(kBufferSize)) {
    REXGPU_ERROR("D3D11: shared memory host buffer creation failed");
    Shutdown();
    return false;
  }
  return true;
}

void D3D11SharedMemory::Shutdown(bool from_destructor) {
  upload_sink_->DestroyHostBuffer();
  // If calling from the destructor, the SharedMemory destructor will call
  // ShutdownCommon.
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void D3D11SharedMemory::ClearCache() {
  SharedMemory::ClearCache();
}

bool D3D11SharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  for (const std::pair<uint32_t, uint32_t>& upload_range : upload_page_ranges) {
    uint32_t offset = upload_range.first << page_size_log2();
    uint32_t remaining = upload_range.second << page_size_log2();
    while (remaining != 0) {
      uint32_t chunk_bytes = std::min(remaining, kSharedMemoryUploadChunkBytes);
      // Mark the pages valid before the copy: a CPU write during or after it
      // then clears the bit and the range is re-uploaded, while marking after
      // the copy would lose that invalidation.
      MakeRangeValid(offset, chunk_bytes, false);
      if (!upload_sink_->UploadToHostBuffer(
              offset, memory().TranslatePhysical(offset), chunk_bytes)) {
        REXGPU_ERROR("D3D11: shared memory upload failed for {:08X}+{}",
                     offset, chunk_bytes);
        // The valid bits set above don't reflect the mirror. Drop the
        // CPU-uploaded validity (keeping GPU-written data, which the CPU
        // must not overwrite) so the range is retried in full later.
        SetSystemPageBlocksValidWithGpuDataWritten();
        return false;
      }
      offset += chunk_bytes;
      remaining -= chunk_bytes;
    }
  }
  return true;
}

}  // namespace rex::graphics::d3d11
