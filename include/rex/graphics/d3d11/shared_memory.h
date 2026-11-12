/**
 * @file        graphics/d3d11/shared_memory.h
 * @brief       D3D11 backend: shared memory page uploads (Phase 2 foundation)
 *
 *              Implements the guest -> host GPU mirror upload policy on top
 *              of the device-free D3D11SharedMemoryUploadSink seam, so the
 *              whole path is unit-testable without a D3D11 device.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <utility>
#include <vector>

#include <rex/graphics/d3d11/shared_memory_upload.h>
#include <rex/graphics/shared_memory.h>
#include <rex/memory.h>

namespace rex::graphics::d3d11 {

class D3D11SharedMemory : public SharedMemory {
 public:
  // Takes ownership of the host mirror endpoint. Production passes the D3D11
  // device sink (shared_memory_upload_device.h); tests pass a recording fake.
  D3D11SharedMemory(memory::Memory& memory,
                    std::unique_ptr<D3D11SharedMemoryUploadSink> upload_sink);
  ~D3D11SharedMemory() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  void ClearCache() override;

  D3D11SharedMemoryUploadSink* upload_sink() const { return upload_sink_.get(); }

 protected:
  bool UploadRanges(
      const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;

 private:
  std::unique_ptr<D3D11SharedMemoryUploadSink> upload_sink_;
};

}  // namespace rex::graphics::d3d11
