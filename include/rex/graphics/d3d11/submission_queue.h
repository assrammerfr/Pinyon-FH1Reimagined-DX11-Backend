/**
 * @file        graphics/d3d11/submission_queue.h
 * @brief       Submission and fence bookkeeping for the D3D11 command lists (Phase 3 M0)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

struct ID3D11DeviceContext;

namespace rex::graphics::d3d11 {

// Replays recorded lists on the single immediate context under one mutex and
// tracks the F_frame/F_swap/F_present values. On the D3D11 path the fence is
// the existing D3D11_QUERY_EVENT; on the native path the same values become
// ID3D12Fence signals. Phase 3 M0 reserves the type and its context; M1 adds
// the replay, query and deferred-release machinery. Instantiated only with the
// command-list flag on.
class D3D11SubmissionQueue {
 public:
  explicit D3D11SubmissionQueue(ID3D11DeviceContext* context);
  ~D3D11SubmissionQueue();

  D3D11SubmissionQueue(const D3D11SubmissionQueue&) = delete;
  D3D11SubmissionQueue& operator=(const D3D11SubmissionQueue&) = delete;

  // Drops queued bookkeeping and waits for any outstanding submitted work.
  void Reset();

  ID3D11DeviceContext* context() const { return context_; }

 private:
  ID3D11DeviceContext* context_ = nullptr;
};

}  // namespace rex::graphics::d3d11
