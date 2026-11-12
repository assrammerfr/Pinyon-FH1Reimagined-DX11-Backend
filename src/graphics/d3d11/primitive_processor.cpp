/**
 * @file        graphics/d3d11/primitive_processor.cpp
 * @brief       D3D11 backend primitive/index processing (Phase 3: real draws)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/primitive_processor.h>

#include <algorithm>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/graphics/d3d11/command_processor.h>
#include <rex/logging.h>
#include <rex/ui/d3d11/d3d11_provider.h>

namespace rex::graphics::d3d11 {

D3D11PrimitiveProcessor::~D3D11PrimitiveProcessor() {
  Shutdown(true);
}

bool D3D11PrimitiveProcessor::Initialize() {
  // Feature level 11_0 has full 32-bit index support; triangle fans and line
  // loops have no host equivalent (converted on the CPU); quads, point
  // sprites and rectangle lists are emulated through geometry shaders / the
  // vertex shader expansions in the DXBC translator - same capabilities as
  // the Direct3D 12 backend.
  if (!InitializeCommon(true, false, false, true, true, true)) {
    Shutdown();
    return false;
  }
  return true;
}

void D3D11PrimitiveProcessor::Shutdown(bool from_destructor) {
  frame_index_buffers_.clear();
  if (frame_index_page_mapped_ != kPageNone) {
    command_processor_.GetD3D11Provider().GetImmediateContext()->Unmap(
        frame_index_pages_[frame_index_page_mapped_].Get(), 0);
    frame_index_page_mappings_[frame_index_page_mapped_] = nullptr;
    frame_index_page_mapped_ = kPageNone;
  }
  frame_index_pages_.clear();
  frame_index_page_mappings_.clear();
  frame_index_page_used_ = 0;
  frame_index_page_next_ = kPageNone;
  builtin_index_buffer_.Reset();
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void D3D11PrimitiveProcessor::EndFrame() {
  ClearPerFrameCache();
  frame_index_buffers_.clear();
  ID3D11DeviceContext* context = command_processor_.GetD3D11Provider().GetImmediateContext();
  if (frame_index_page_mapped_ != kPageNone) {
    context->Unmap(frame_index_pages_[frame_index_page_mapped_].Get(), 0);
    frame_index_page_mappings_[frame_index_page_mapped_] = nullptr;
    frame_index_page_mapped_ = kPageNone;
  }
  // Start over from the first ring page; pages are kept (their contents only
  // matter within a frame) and DO_NOT_OVERWRITE keeps the driver from
  // handing out regions with GPU writes in flight.
  frame_index_page_used_ = 0;
  frame_index_page_next_ = kPageNone;
  if (context) {
    context->Flush();
  }
}

bool D3D11PrimitiveProcessor::InitializeBuiltinIndexBuffer(
    size_t size_bytes, std::function<void(void*)> fill_callback) {
  assert_not_zero(size_bytes);
  assert_true(!builtin_index_buffer_);

  std::vector<uint8_t> initial_data(size_bytes);
  fill_callback(initial_data.data());

  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = UINT(size_bytes);
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
  desc.CPUAccessFlags = 0;
  desc.MiscFlags = 0;
  desc.StructureByteStride = 0;
  D3D11_SUBRESOURCE_DATA initial = {};
  initial.pSysMem = initial_data.data();
  initial.SysMemPitch = 0;
  initial.SysMemSlicePitch = 0;
  if (FAILED(command_processor_.GetD3D11Provider().GetDevice()->CreateBuffer(
          &desc, &initial, &builtin_index_buffer_))) {
    REXGPU_ERROR("D3D11 primitive processor: failed to create the built-in index buffer "
                 "with {} bytes",
                 size_bytes);
    return false;
  }
  return true;
}

void* D3D11PrimitiveProcessor::RequestHostConvertedIndexBufferForCurrentFrame(
    xenos::IndexFormat format, uint32_t index_count, bool coaindex_for_simd,
    uint32_t coalignment_original_address, size_t& backend_handle_out) {
  const size_t index_size = format == xenos::IndexFormat::kInt16 ? sizeof(uint16_t) : sizeof(uint32_t);
  const uint32_t size_bytes =
      uint32_t(index_size * index_count) +
      (coaindex_for_simd ? uint32_t(XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE) : 0);
  if (size_bytes > kIndexRingPageBytes) {
    REXGPU_ERROR("D3D11 primitive processor: converted index buffer of {} bytes exceeds the "
                 "ring page size",
                 size_bytes);
    return nullptr;
  }
  ID3D11Device* device = command_processor_.GetD3D11Provider().GetDevice();
  ID3D11DeviceContext* context = command_processor_.GetD3D11Provider().GetImmediateContext();
  // Advance to a page with room (or a brand-new one).
  if (frame_index_page_next_ == kPageNone) {
    frame_index_page_next_ = 0;
    frame_index_page_used_ = 0;
  } else if (frame_index_page_used_ + size_bytes > kIndexRingPageBytes) {
    frame_index_page_next_ = frame_index_page_next_ + 1;
    frame_index_page_used_ = 0;
  }
  if (frame_index_page_next_ >= frame_index_pages_.size()) {
    Microsoft::WRL::ComPtr<ID3D11Buffer> page;
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = kIndexRingPageBytes;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &page))) {
      REXGPU_ERROR("D3D11 primitive processor: failed to create an index ring page");
      return nullptr;
    }
    frame_index_pages_.push_back(std::move(page));
    frame_index_page_mappings_.push_back(nullptr);
  }
  // One persistent DO_NOT_OVERWRITE mapping per active page: the base class
  // writes through the returned pointer after this call returns, and the
  // draw binds the buffer later in the same frame. Unmap the previously
  // active page before mapping the new one (a D3D11 buffer holds a single
  // Map at a time).
  if (frame_index_page_mapped_ != frame_index_page_next_) {
    if (frame_index_page_mapped_ != kPageNone) {
      context->Unmap(frame_index_pages_[frame_index_page_mapped_].Get(), 0);
      frame_index_page_mappings_[frame_index_page_mapped_] = nullptr;
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(frame_index_pages_[frame_index_page_next_].Get(), 0,
                            D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped))) {
      REXGPU_ERROR("D3D11 primitive processor: failed to map the index ring page");
      frame_index_page_mapped_ = kPageNone;
      return nullptr;
    }
    frame_index_page_mapped_ = frame_index_page_next_;
    frame_index_page_mappings_[frame_index_page_next_] = mapped.pData;
    frame_index_page_used_ = 0;
  }
  uint8_t* page_start =
      static_cast<uint8_t*>(frame_index_page_mappings_[frame_index_page_next_]);
  uint8_t* mapping = page_start + frame_index_page_used_;
  if (coaindex_for_simd) {
    mapping += GetSimdCoalignmentOffset(mapping, coalignment_original_address);
  }
  const uint32_t offset = uint32_t(mapping - page_start);
  frame_index_page_used_ = offset + size_bytes;
  backend_handle_out = frame_index_buffers_.size();
  FrameIndexBuffer buffer;
  buffer.buffer = frame_index_pages_[frame_index_page_next_].Get();
  buffer.offset = offset;
  frame_index_buffers_.push_back(buffer);
  return mapping;
}

}  // namespace rex::graphics::d3d11
