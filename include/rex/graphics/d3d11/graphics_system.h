/**
 * @file        graphics/d3d11/graphics_system.h
 * @brief       Direct3D 11 GPU backend (Phase 1 skeleton: draws nothing)
 *
 *              Third GPU backend of the shiftglue-sdk plugin factory
 *              ("null" / "d3d12" / "vulkan" / "d3d11"), targeted at
 *              Direct3D feature level 11_0 hardware without Vulkan support
 *              (NVIDIA Fermi). The guest GPU side runs, but until the later
 *              porting phases land, draws, resolves and shaders are logged
 *              and dropped - the same guest-correctness posture as the null
 *              backend, with a real D3D11 device, swap chain and presenter
 *              behind it so cleared frames present continuously.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <string>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>

namespace rex::graphics::d3d11 {

class D3D11SharedMemory;
class D3D11PrimitiveProcessor;
class D3D11TextureCache;
class D3D11RenderTargetCache;

class D3D11GraphicsSystem : public GraphicsSystem {
 public:
  D3D11GraphicsSystem();
  ~D3D11GraphicsSystem() override;

  std::string name() const override;

 protected:
  void CreateProvider(bool with_presentation) override;
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::graphics::d3d11
