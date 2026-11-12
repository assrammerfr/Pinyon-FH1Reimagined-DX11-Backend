/**
 * @file        graphics/d3d11/resource_state_tracker.h
 * @brief       Resource-state tracking for the D3D11 command-list path (Phase 3 M1)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <unordered_map>

#include <d3d11.h>
#include <wrl/client.h>

#include <rex/graphics/d3d11/command_list.h>

namespace rex::graphics::d3d11 {

// Tracks the Phase 2 resource states and emits transition/UAV barrier packets
// into the command list. The D3D11 replay treats transitions as no-ops, but the
// recorded stream must still carry them for the native mapping and for
// validation. The map holds a reference to every tracked resource until Reset
// (called at the end of the frame), so entries are frame-scoped by design.
class D3D11ResourceStateTracker {
 public:
  D3D11ResourceStateTracker();
  ~D3D11ResourceStateTracker();

  D3D11ResourceStateTracker(const D3D11ResourceStateTracker&) = delete;
  D3D11ResourceStateTracker& operator=(const D3D11ResourceStateTracker&) = delete;

  // Drops every tracked resource, for shutdown or a device reset.
  void Reset();

  // Registers a resource with a known state without recording a barrier.
  // Returns false for a null resource; returns true when the resource was
  // adopted (an already-tracked resource keeps its current state).
  bool Track(IUnknown* resource, D3D11ResourceState initial_state);

  // Releases the tracker's reference to a resource. Its state is forgotten.
  void Forget(IUnknown* resource);

  bool IsTracked(IUnknown* resource) const;
  D3D11ResourceState State(IUnknown* resource) const;

  // Records a transition when the state changes. An untracked resource is
  // adopted first and still gets a barrier from kCommon, so the tape's first
  // use is explicit. Returns true when a barrier packet was appended; false
  // for a same-state request or a full tape.
  bool Transition(D3D11CommandList& tape, IUnknown* resource, D3D11ResourceState new_state);

  // Records an unconditional UAV barrier: unordered-access writes are not
  // covered by the state map, matching D3D11's requirement that the same UAV
  // is not read and written in one dispatch without one.
  bool TransitionUav(D3D11CommandList& tape, IUnknown* resource);

  size_t tracked_count() const { return resources_.size(); }
  uint32_t transition_count(IUnknown* resource) const;
  uint32_t recorded_barrier_count() const { return recorded_barrier_count_; }
  uint32_t recorded_uav_barrier_count() const { return recorded_uav_barrier_count_; }

 private:
  struct Entry {
    Microsoft::WRL::ComPtr<IUnknown> resource;
    D3D11ResourceState state = D3D11ResourceState::kCommon;
    uint32_t transitions = 0;
  };

  bool AppendBarrier(D3D11CommandList& tape, IUnknown* resource, D3D11ResourceState before,
                     D3D11ResourceState after);

  std::unordered_map<IUnknown*, Entry> resources_;
  uint32_t recorded_barrier_count_ = 0;
  uint32_t recorded_uav_barrier_count_ = 0;
};

}  // namespace rex::graphics::d3d11
