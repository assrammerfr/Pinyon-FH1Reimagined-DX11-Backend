/**
 * @file        graphics/d3d11/graphics_system.cpp
 * @brief       Direct3D 11 GPU backend skeleton (Phase 1)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d11/graphics_system.h>

#include <rex/graphics/d3d11/command_processor.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/d3d11/d3d11_provider.h>

namespace rex::graphics::d3d11 {

D3D11GraphicsSystem::D3D11GraphicsSystem() = default;

D3D11GraphicsSystem::~D3D11GraphicsSystem() = default;

std::string D3D11GraphicsSystem::name() const {
  return "Direct3D 11";
}

void D3D11GraphicsSystem::CreateProvider(bool /*with_presentation*/) {
  // Like D3D12, the provider is device-level and swap chains are created
  // lazily per-window by the presenter, so headless and presentation
  // providers are the same object.
  provider_ = rex::ui::d3d11::D3D11Provider::Create();
}

std::unique_ptr<CommandProcessor> D3D11GraphicsSystem::CreateCommandProcessor() {
  return std::unique_ptr<CommandProcessor>(new D3D11CommandProcessor(this, kernel_state_));
}

}  // namespace rex::graphics::d3d11
