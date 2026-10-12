// Draws the finished frame into the presenter's guest output image.
//
// The SDK's Vulkan presenter makes that image for sampling, storage and as a
// colour attachment, without TRANSFER_DST usage, so a copy into it breaks the
// Vulkan rules (the Khronos layer's VUID-vkCmdCopyImage-dstImage and
// VUID-VkImageMemoryBarrier-oldLayout-01213). A full-screen triangle through
// the blit shader writes it as a colour attachment instead.
#pragma once

#include <array>
#include <unordered_map>

#include <nvrhi/nvrhi.h>

namespace nr {

class OutputBlit {
 public:
  explicit OutputBlit(nvrhi::IDevice* device) : device_(device) {}

  // Records the draw of `source` (texel for texel, width x height from the
  // top left) into `target`, which must be a render target in the
  // RenderTarget state and stay there (keepInitialState). Ends with the
  // render pass closed, so the caller may record its own barriers next.
  bool Record(nvrhi::ICommandList* command_list, nvrhi::ITexture* source, nvrhi::ITexture* target,
              uint32_t width, uint32_t height);

  // Before the wrapped targets go away.
  void Forget(nvrhi::ITexture* target);

 private:
  bool Initialize();

  nvrhi::IDevice* device_;
  bool tried_ = false;
  nvrhi::ShaderHandle vs_, ps_;
  nvrhi::BindingLayoutHandle layout_;
  std::unordered_map<nvrhi::Format, nvrhi::GraphicsPipelineHandle> pipelines_;
  std::unordered_map<nvrhi::ITexture*, nvrhi::FramebufferHandle> framebuffers_;
  std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> sets_;
};

}  // namespace nr
