/**
 * @file        graphics/d3d11/primitive_processor.h
 * @brief       D3D11 backend primitive/index processing (Phase 3: real draws)
 *
 *              Owns the device-side index buffers the shared PrimitiveProcessor
 *              logic fills:
 *              - the built-in index buffer for auto-indexed and adapter-index
 *                primitive conversions (a real DEFAULT buffer uploaded at
 *                initialization),
 *              - per-frame host-converted index buffers (a rotating ring of
 *                DYNAMIC buffers mapped with DO_NOT_OVERWRITE per request;
 *                guest triangle fans / line loops are the typical sources).
 *              Guest DMA indices bind straight from the shared-memory mirror
 *                (it has BIND_INDEX_BUFFER) in the command processor.
 *
 *              Mirrors the Direct3D 12 backend's D3D12PrimitiveProcessor;
 *              vertex fetch is in-shader, so indices are all this handles.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

#include <rex/graphics/primitive_processor.h>
#include <rex/ui/d3d11/d3d11_api.h>

namespace rex::graphics::d3d11 {

class D3D11CommandProcessor;

class D3D11PrimitiveProcessor final : public PrimitiveProcessor {
 public:
  D3D11PrimitiveProcessor(const RegisterFile& register_file, memory::Memory& memory,
                          SharedMemory& shared_memory, D3D11CommandProcessor& command_processor)
      : PrimitiveProcessor(register_file, memory, shared_memory),
        command_processor_(command_processor) {}
  ~D3D11PrimitiveProcessor() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  // Frame boundary (the swap): converted buffers of the finished frame are
  // no longer written, and the shared per-frame conversion cache is dropped.
  void EndFrame();

  ID3D11Buffer* GetBuiltinIndexBuffer() const { return builtin_index_buffer_.Get(); }
  // Byte offset of a builtin index buffer allocation within the buffer
  // (mirrors D3D12PrimitiveProcessor::GetBuiltinIndexBufferGpuAddress, which
  // wraps the same protected base-class helper).
  uint32_t GetBuiltinIndexBufferOffset(size_t handle) const {
    return uint32_t(GetBuiltinIndexBufferOffsetBytes(handle));
  }
  // Converted buffers are per-frame ring allocations: the handle selects the
  // allocation, the byte offset comes along.
  ID3D11Buffer* GetConvertedIndexBuffer(size_t handle) const {
    return frame_index_buffers_[handle].buffer.Get();
  }
  uint32_t GetConvertedIndexBufferOffset(size_t handle) const {
    return frame_index_buffers_[handle].offset;
  }

 protected:
  bool InitializeBuiltinIndexBuffer(size_t size_bytes,
                                    std::function<void(void*)> fill_callback) override;
  void* RequestHostConvertedIndexBufferForCurrentFrame(xenos::IndexFormat format,
                                                       uint32_t index_count, bool coaindex_for_simd,
                                                       uint32_t coalignment_original_address,
                                                       size_t& backend_handle_out) override;

 private:
  D3D11CommandProcessor& command_processor_;

  Microsoft::WRL::ComPtr<ID3D11Buffer> builtin_index_buffer_;

  // Rotating ring of DYNAMIC index buffers for converted per-frame indices.
  static constexpr uint32_t kIndexRingPageBytes = 1 << 20;
  struct FrameIndexBuffer {
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    uint32_t offset = 0;
  };
  std::deque<FrameIndexBuffer> frame_index_buffers_;
  std::vector<Microsoft::WRL::ComPtr<ID3D11Buffer>> frame_index_pages_;
  // Mapped pData of each page while mapped (nullptr when unmapped).
  std::vector<void*> frame_index_page_mappings_;
  uint32_t frame_index_page_used_ = 0;
  // The page allocated from; -1 = none yet.
  size_t frame_index_page_next_ = SIZE_MAX;
  // The page currently CPU-mapped (one persistent DO_NOT_OVERWRITE mapping
  // per active page; a D3D11 buffer holds a single Map at a time). -1 = none.
  size_t frame_index_page_mapped_ = SIZE_MAX;
  static constexpr size_t kPageNone = SIZE_MAX;
};

}  // namespace rex::graphics::d3d11
