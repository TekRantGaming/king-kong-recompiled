// Depth of field on Jack's first-person hands and what he holds (docs/lighting.md): they sit close to the eye and go soft,
// the world stays sharp, as in a film or Killzone Shadow Fall. Off by default.
//
//  - The game draws their colour in one run of draws. Each of those draws is made again (game_hands_dof.cpp) into a mask,
//    with shaders/hands_dof_mask.hlsl: 0 away from them, else how soft each pixel is, from its distance.
//  - At the first other draw after that run a full-screen pass (shaders/hands_dof.hlsl) gathers over a disc from a copy of
//    the picture. Their edges spread over the world behind them; smoke, flames and the HUD are drawn later and stay sharp.
//
// The game renderer calls it from small hooks (game_hands_dof.cpp). NVRHI only; the render thread.
#pragma once

#include <cstdint>
#include <map>
#include <utility>

#include <nvrhi/nvrhi.h>

#include "backend/ao_timer.h"
#include "backend/hands_dof_config.h"

namespace nr {

class HandsDof {
 public:
  // The largest blur radius, as a share of the picture's height (shaders/hands_dof.hlsl).
  static constexpr float kRadius = 0.006f;

  explicit HandsDof(nvrhi::IDevice* device) : device_(device) {}
  bool Initialize();
  void SetTimer(AoGpuTimer* timer) { timer_ = timer; }
  void ForgetTargets();

  // The framebuffer for a hands draw made again: the mask, and the depth the game draw used, read only.
  nvrhi::IFramebuffer* MaskFramebuffer(nvrhi::ICommandList* cl, const nvrhi::FramebufferDesc& game, bool first_this_frame);
  // The gather over `color`.
  bool Apply(nvrhi::ICommandList* cl, nvrhi::ITexture* color);

 private:
  nvrhi::IFramebuffer* Framebuffer(nvrhi::ITexture* color);
  nvrhi::IGraphicsPipeline* Pipeline(const nvrhi::FramebufferInfo& info);

  nvrhi::IDevice* device_;
  AoGpuTimer* timer_ = nullptr;
  nvrhi::ShaderHandle vs_, ps_;
  nvrhi::BindingLayoutHandle layout_;
  nvrhi::BufferHandle constants_;
  nvrhi::TextureHandle mask_, copy_;
  nvrhi::BindingSetHandle set_;
  std::map<nvrhi::ITexture*, nvrhi::FramebufferHandle> mask_framebuffers_;  // by depth
  std::map<nvrhi::ITexture*, nvrhi::FramebufferHandle> framebuffers_;
  std::map<uint32_t, nvrhi::GraphicsPipelineHandle> pipelines_;
};

}  // namespace nr
