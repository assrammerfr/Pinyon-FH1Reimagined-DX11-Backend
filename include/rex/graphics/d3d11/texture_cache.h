/**
 * @file        graphics/d3d11/texture_cache.h
 * @brief       D3D11 backend texture cache (Phase 2: CPU untiling + uploads)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/d3d11/d3d11_api.h>

namespace rex::graphics::d3d11 {

class D3D11CommandProcessor;

class D3D11TextureCache final : public TextureCache {
 public:
  using TextureCache::TextureKey;

  D3D11TextureCache(const RegisterFile& register_file, SharedMemory& shared_memory,
                    uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y,
                    D3D11CommandProcessor& command_processor)
      : TextureCache(register_file, shared_memory, draw_resolution_scale_x, draw_resolution_scale_y),
        command_processor_(command_processor) {}
  ~D3D11TextureCache() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  // Phase 3 binding: the SRV a shader's texture binding resolves to as of the
  // latest RequestTextures (nullptr for an unbound slot - reads as zeros, like
  // the Direct3D 12 null descriptors). The signed view is created on demand.
  ID3D11ShaderResourceView* GetActiveTextureBindfulSRV(const DxbcShader::TextureBinding& binding);

  // The guest frontbuffer as a host texture for presentation - texture fetch
  // constant 0 from the swap packet, loaded from the guest memory the resolve
  // path wrote (the Direct3D 12 texture cache's RequestSwapTexture, SRV
  // flavour). nullptr if the fetch doesn't describe a 2D texture or the
  // format cannot be hosted.
  ID3D11ShaderResourceView* RequestSwapTexture(xenos::TextureFormat& format_out,
                                               uint32_t& width_out, uint32_t& height_out);

  // The guest byte range the swap fetch (texture fetch constant 0) describes -
  // the frontbuffer's guest address and the size of its base level in guest
  // memory. This is the extent fh1_d3d11_resolve_readback pulls from the GPU
  // mirror at the swap, so guest memory holds exactly the frame being
  // presented. False when the fetch isn't a 2D texture that can be hosted.
  bool GetSwapGuestExtent(uint32_t& base_out, uint32_t& length_out);

  // The resource the last RequestSwapTexture uploaded (borrowed - the cache
  // owns it; null before the first swap), for fh1_debug_presented_pair: the
  // presented leg exactly as the presenter paints it, after the untile and
  // upload, at the swap that presents it.
  ID3D11Resource* swap_resource() const { return swap_resource_; }

  // Diagnostic (fh1_debug_texture_dump): the 2D texture the cache holds for
  // this guest base address, with the typed format its SRV samples as and the
  // resource's stored extent - exactly the texels a shader sampling that
  // address reads, before any draw composes them. Borrowed: the cache owns the
  // texture, so the pointer is valid only until the texture is evicted.
  // `guest_format` disambiguates textures sharing a base (the cache keeps one
  // per fetch key, and a stale format sibling is not what the draw samples).
  ID3D11Texture2D* FindTexture2DForGuestBase(uint32_t guest_base, xenos::TextureFormat guest_format,
                                             DXGI_FORMAT& read_format_out, uint32_t& width_out,
                                             uint32_t& height_out);
  // As above without the format filter, for a fetch whose format the cache has
  // no entry for: proves whether the base is held at all, and under which
  // format, instead of reporting "not in the cache" when it is.
  ID3D11Texture2D* FindAnyTexture2DForGuestBase(uint32_t guest_base, DXGI_FORMAT& read_format_out,
                                                uint32_t& width_out, uint32_t& height_out,
                                                uint32_t& host_format_out);

  // Sampler parameters that can be directly converted to a host sampler.
  union SamplerParameters {
    uint32_t value;
    struct {
      xenos::ClampMode clamp_x : 3;         // 3
      xenos::ClampMode clamp_y : 3;         // 6
      xenos::ClampMode clamp_z : 3;         // 9
      xenos::BorderColor border_color : 2;  // 11
      // For anisotropic, these are true.
      uint32_t mag_linear : 1;              // 12
      uint32_t min_linear : 1;              // 13
      uint32_t mip_linear : 1;              // 14
      xenos::AnisoFilter aniso_filter : 3;  // 17
      uint32_t mip_min_level : 4;           // 21
      uint32_t mip_base_map : 1;            // 22
      // Maximum mip level is in the texture resource itself, but mip_base_map
      // can be used to limit fetching to mip_min_level.
    };

    SamplerParameters() : value(0) { static_assert_size(*this, sizeof(value)); }
    bool operator==(const SamplerParameters& parameters) const { return value == parameters.value; }
    bool operator!=(const SamplerParameters& parameters) const { return value != parameters.value; }
  };
  // The host sampler description for a shader sampler binding (port of the
  // Direct3D 12 texture cache's GetSamplerParameters + WriteSampler).
  D3D11_SAMPLER_DESC GetSamplerDesc(const DxbcShader::SamplerBinding& binding) const;

  static constexpr bool AreDimensionsCompatible(xenos::FetchOpDimension binding_dimension,
                                                xenos::DataDimension resource_dimension) {
    switch (binding_dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        return resource_dimension == xenos::DataDimension::k1D ||
               resource_dimension == xenos::DataDimension::k2DOrStacked ||
               resource_dimension == xenos::DataDimension::k3D;
      case xenos::FetchOpDimension::k3DOrStacked:
        return resource_dimension == xenos::DataDimension::k3D;
      case xenos::FetchOpDimension::kCube:
        return resource_dimension == xenos::DataDimension::kCube;
      default:
        return false;
    }
  }

 protected:
  uint32_t GetHostFormatSwizzle(TextureKey key) const override;
  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const override;
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension dimension) const override;
  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;
  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                             bool load_mips) override;

 private:
  // The host format table (DXGI mappings, swizzles, CPU converters) lives in
  // the .cpp next to the load path that consumes it.

  class D3D11Texture final : public Texture {
   public:
    D3D11Texture(D3D11TextureCache& texture_cache, const TextureKey& key,
                 ID3D11Resource* resource, ID3D11ShaderResourceView* srv,
                 uint64_t host_memory_usage, bool track_usage = true);
    ~D3D11Texture() override;

    ID3D11Resource* resource() const { return resource_.Get(); }
    // Unsigned normalized (or float) view of the resource - the Phase 3
    // binding path uses it directly, the signed one is created there on demand.
    ID3D11ShaderResourceView* srv() const { return srv_.Get(); }
    // On-demand variant of the main view (signed, or a stacked texture viewed
    // as 2D): (is_signed ? 1 : 0) | (special ? 2 : 0) it was created for.
    ID3D11ShaderResourceView* variant_srv() const { return variant_srv_.Get(); }
    uint32_t variant_key() const { return variant_key_; }
    void set_variant(uint32_t variant_key, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv) {
      variant_key_ = variant_key;
      variant_srv_ = std::move(srv);
    }

   private:
    Microsoft::WRL::ComPtr<ID3D11Resource> resource_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> variant_srv_;
    uint32_t variant_key_ = 0;
  };

  static bool IsDecompressionNeeded(xenos::TextureFormat format, uint32_t width, uint32_t height);

  xenos::ClampMode NormalizeClampMode(xenos::ClampMode clamp_mode) const;

  void LogUnsupportedFormatOnce(TextureKey key);

  D3D11CommandProcessor& command_processor_;

  // CPU-authoritative copies of the guest base and mips spans (the same spans
  // RequestRanges made resident), reused across loads.
  std::vector<uint8_t> load_scratch_base_;
  std::vector<uint8_t> load_scratch_mips_;
  // Untiled + converted image of one guest level slice (all Z slices for 3D).
  std::vector<uint8_t> staging_;

  // fh1_debug_presented_pair reads this after the swap's upload.
  ID3D11Resource* swap_resource_ = nullptr;

  // RequestSwapTexture explains the first failed frontbuffer request once.
  bool swap_request_logged_ = false;
  uint32_t swap_fail_logs_ = 0;
  uint64_t unsupported_format_mask_logged_ = 0;
  uint32_t texture_uploads_logged_ = 0;
  uint64_t texture_upload_count_ = 0;
  uint64_t texture_upload_bytes_ = 0;
};

}  // namespace rex::graphics::d3d11
