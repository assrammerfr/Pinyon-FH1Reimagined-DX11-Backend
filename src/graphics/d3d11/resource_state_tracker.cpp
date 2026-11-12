/**
 * @file        graphics/d3d11/resource_state_tracker.cpp
 * @brief       Resource-state tracking for the D3D11 command-list path (Phase 3 M1)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/resource_state_tracker.h>

namespace rex::graphics::d3d11 {

D3D11ResourceStateTracker::D3D11ResourceStateTracker() = default;

D3D11ResourceStateTracker::~D3D11ResourceStateTracker() = default;

void D3D11ResourceStateTracker::Reset() {
  resources_.clear();
  recorded_barrier_count_ = 0;
  recorded_uav_barrier_count_ = 0;
}

bool D3D11ResourceStateTracker::Track(IUnknown* resource, D3D11ResourceState initial_state) {
  if (!resource) {
    return false;
  }
  auto [it, inserted] = resources_.try_emplace(resource);
  if (inserted) {
    it->second.resource = resource;
    it->second.state = initial_state;
  }
  return true;
}

void D3D11ResourceStateTracker::Forget(IUnknown* resource) {
  if (resource) {
    resources_.erase(resource);
  }
}

bool D3D11ResourceStateTracker::IsTracked(IUnknown* resource) const {
  return resource && resources_.find(resource) != resources_.end();
}

D3D11ResourceState D3D11ResourceStateTracker::State(IUnknown* resource) const {
  auto it = resource ? resources_.find(resource) : resources_.end();
  return it == resources_.end() ? D3D11ResourceState::kCommon : it->second.state;
}

uint32_t D3D11ResourceStateTracker::transition_count(IUnknown* resource) const {
  auto it = resource ? resources_.find(resource) : resources_.end();
  return it == resources_.end() ? 0 : it->second.transitions;
}

bool D3D11ResourceStateTracker::AppendBarrier(D3D11CommandList& tape, IUnknown* resource,
                                              D3D11ResourceState before, D3D11ResourceState after) {
  D3D11BarrierPacket* packet = tape.Append<D3D11BarrierPacket>();
  if (!packet) {
    return false;
  }
  packet->resource = resource;
  packet->before = before;
  packet->after = after;
  ++recorded_barrier_count_;
  return true;
}

bool D3D11ResourceStateTracker::Transition(D3D11CommandList& tape, IUnknown* resource,
                                           D3D11ResourceState new_state) {
  if (!resource) {
    return false;
  }
  auto [it, inserted] = resources_.try_emplace(resource);
  if (inserted) {
    // First use: adopt it, but still put the first state on the tape so the
    // stream is self-describing for the native mapping.
    it->second.resource = resource;
    if (!AppendBarrier(tape, resource, D3D11ResourceState::kCommon, new_state)) {
      resources_.erase(it);
      return false;
    }
    it->second.state = new_state;
    ++it->second.transitions;
    return true;
  }
  if (it->second.state == new_state) {
    return false;
  }
  if (!AppendBarrier(tape, resource, it->second.state, new_state)) {
    return false;
  }
  it->second.state = new_state;
  ++it->second.transitions;
  return true;
}

bool D3D11ResourceStateTracker::TransitionUav(D3D11CommandList& tape, IUnknown* resource) {
  if (!resource) {
    return false;
  }
  D3D11UavBarrierPacket* packet = tape.Append<D3D11UavBarrierPacket>();
  if (!packet) {
    return false;
  }
  packet->resource = resource;
  ++recorded_uav_barrier_count_;
  return true;
}

}  // namespace rex::graphics::d3d11
