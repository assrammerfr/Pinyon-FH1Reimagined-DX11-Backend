/**
 * @file        graphics/d3d11/command_list.cpp
 * @brief       Deferred command tape for the D3D11 backend (Phase 3 M1)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/command_list.h>

#include <algorithm>
#include <cstring>

namespace rex::graphics::d3d11 {

namespace {

size_t AlignUp(size_t value, size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace

D3D11CommandList::D3D11CommandList(size_t max_packets, size_t max_arena_bytes)
    : max_packets_(max_packets), max_arena_bytes_(max_arena_bytes) {}

D3D11CommandList::~D3D11CommandList() = default;

bool D3D11CommandList::AppendBytes(const void* data, size_t size, D3D11ArenaSpan& span_out) {
  span_out = {};
  if (size == 0) {
    return true;
  }
  if (data == nullptr) {
    overflow_ = true;
    return false;
  }
  const size_t offset = AlignUp(arena_.size(), kArenaAlignment);
  if (size > max_arena_bytes_ || offset > max_arena_bytes_ - size) {
    overflow_ = true;
    return false;
  }
  arena_.resize(offset + size);
  std::memcpy(arena_.data() + offset, data, size);
  span_out.offset = uint32_t(offset);
  span_out.size = uint32_t(size);
  return true;
}

bool D3D11CommandList::AppendUploadBufferPacket(ID3D11Buffer* destination,
                                                uint32_t destination_offset, const void* data,
                                                uint32_t size) {
  if (!destination || size == 0 || data == nullptr) {
    return false;
  }
  D3D11UploadBufferPacket* packet = Append<D3D11UploadBufferPacket>();
  if (!packet) {
    return false;
  }
  D3D11ArenaSpan span;
  if (!AppendBytes(data, size, span)) {
    commands_.pop_back();
    return false;
  }
  packet->destination = destination;
  packet->destination_offset = destination_offset;
  packet->data = span;
  return true;
}

const uint8_t* D3D11CommandList::Data(D3D11ArenaSpan span) const {
  if (span.size == 0) {
    return nullptr;
  }
  if (span.offset > arena_.size() || span.size > arena_.size() - span.offset) {
    return nullptr;
  }
  return arena_.data() + span.offset;
}

void D3D11CommandList::Truncate(size_t size) {
  if (size < commands_.size()) {
    commands_.resize(size);
  }
}

void D3D11CommandList::Replay(D3D11CommandSink& sink) const {
  for (const D3D11Command& command : commands_) {
    sink.Execute(command, *this);
  }
}

void D3D11CommandList::Reset() {
  commands_.clear();
  arena_.clear();
  overflow_ = false;
}

}  // namespace rex::graphics::d3d11
