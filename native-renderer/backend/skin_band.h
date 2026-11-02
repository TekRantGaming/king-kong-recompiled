// A slight red band on human skin where the lit side turns into the shadow side (docs/lighting.md): what light scattered
// under the skin looks like there. Only the band: nothing is translucent, nothing blurs.
//
//  - A human's skin is known by its base texture (SkinTexture). Each of those colour draws is made again (game_skin.cpp)
//    with shaders/skin_band_mask.hlsl: the texture's own colour, and how much of that texel is skin (their atlases also hold
//    hair, cloth and such).
//  - The picture over that colour is how much light each pixel got, whatever the lights were. A full-screen pass
//    (shaders/skin_band.hlsl) finds the darkest and brightest skin around each skin pixel and adds the red between them.
//  - It runs at the fog pass for the world, and again after the first-person hands, which are drawn later.
//
// The game renderer calls it from small hooks (game_skin.cpp). NVRHI only; the render thread.
#pragma once

#include <cstdint>
#include <map>
#include <memory>

#include <nvrhi/nvrhi.h>

#include "backend/ao_timer.h"
#include "backend/skin_band_config.h"

namespace nr {

// Tuning (docs/lighting.md); not settings.
struct SkinBandTuning {
  static constexpr float kStrength = 1.0f;       // the band at its fullest, as shaders/skin_band.hlsl's red
  static constexpr float kReach = 0.03f;         // how far around a pixel it looks, as a share of the picture's height
  static constexpr float kContrast = 0.25f;      // light and shadow this far apart give the full band
};

// The base textures of human skin: the canonical hash of their base level (HostTexture::pack_hash).
bool SkinTexture(uint64_t base_hash);

class SkinBand {
 public:
  explicit SkinBand(nvrhi::IDevice* device);
  ~SkinBand();
  bool Initialize();
  void SetTimer(AoGpuTimer* timer) { timer_ = timer; }
  void ForgetTargets();

  // The framebuffer for a skin draw made again: the skin image, and the depth the game draw used, read only.
  // first: the first since the last Apply.
  nvrhi::IFramebuffer* MaskFramebuffer(nvrhi::ICommandList* cl, const nvrhi::FramebufferDesc& game, bool first);
  // The band into `color`, from the skin drawn since the last Apply.
  bool Apply(nvrhi::ICommandList* cl, nvrhi::ITexture* color);

 private:
  struct Target;
  Target* TargetFor(nvrhi::ITexture* color);
  nvrhi::IFramebuffer* Framebuffer(nvrhi::ITexture* color);
  nvrhi::IGraphicsPipeline* Pipeline(const nvrhi::FramebufferInfo& info);

  nvrhi::IDevice* device_;
  AoGpuTimer* timer_ = nullptr;
  nvrhi::ShaderHandle vs_, ps_;
  nvrhi::BindingLayoutHandle layout_;
  nvrhi::BufferHandle constants_;
  std::map<uint64_t, std::unique_ptr<Target>> targets_;  // by size and format: the scene's, and the hands' with an upscaler
  std::map<nvrhi::ITexture*, nvrhi::FramebufferHandle> framebuffers_;
  std::map<uint32_t, nvrhi::GraphicsPipelineHandle> pipelines_;
};

}  // namespace nr
