/**
 * @file        graphics/d3d11/submission_queue.cpp
 * @brief       Submission and fence bookkeeping for the D3D11 command lists (Phase 3 M0)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/submission_queue.h>

namespace rex::graphics::d3d11 {

D3D11SubmissionQueue::D3D11SubmissionQueue(ID3D11DeviceContext* context) : context_(context) {}

D3D11SubmissionQueue::~D3D11SubmissionQueue() = default;

void D3D11SubmissionQueue::Reset() {}

}  // namespace rex::graphics::d3d11
