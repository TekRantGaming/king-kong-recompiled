// Official SMAA v2.8 1x. All shaders and both lookup images are embedded.
#pragma once
#include <array>
#include <map>
#include <memory>
#include <nvrhi/nvrhi.h>
#include "backend/ao_timer.h"

namespace nr {
class Smaa {
 public:
  explicit Smaa(nvrhi::IDevice* device) : device_(device) {}
  // Creates all three pipelines before gameplay (also called by PipelinePrewarm).
  bool Initialize();
  bool Render(nvrhi::ICommandList* cl, nvrhi::ITexture* source, nvrhi::ITexture* target);
 private:
  nvrhi::IDevice* device_;
  struct Pass {
    nvrhi::ShaderHandle vs, ps;
    nvrhi::BindingLayoutHandle layout;
    std::map<nvrhi::Format, nvrhi::GraphicsPipelineHandle> pipelines;
    nvrhi::BindingSetHandle set;
    nvrhi::FramebufferHandle framebuffer;
  };
  std::array<Pass, 3> passes_;
  nvrhi::TextureHandle area_, search_, edges_, weights_, source_, target_;
  nvrhi::SamplerHandle linear_, point_;
  bool uploaded_ = false;
  std::unique_ptr<AoGpuTimer> timer_;
  nvrhi::GraphicsPipelineHandle Pipeline(unsigned pass, nvrhi::IFramebuffer* framebuffer);
  bool Bind(nvrhi::ITexture* source, nvrhi::ITexture* target);
};
}  // namespace nr
