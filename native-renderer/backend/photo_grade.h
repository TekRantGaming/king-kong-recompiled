// Photo mode's grade pass (task 208, shaders/photo.hlsl, docs/photo-mode.md): exposure, white balance,
// contrast, saturation, the looks, vignette, grain, letterbox bars, the mirror and the depth of field, over
// the finished picture (the frame image the presenter shows and captures read), from a copy of it.
//
// Independent of the game renderer (NVRHI only), recorded on the render thread.
#pragma once

#include <cstdint>
#include <map>

#include <nvrhi/nvrhi.h>

#include "plugin/native_photo.h"

namespace nr {

class PhotoGrade {
 public:
  explicit PhotoGrade(nvrhi::IDevice* device);
  ~PhotoGrade();

  bool Initialize();
  bool ready() const { return ready_; }

  // The view-space depth (metres) the depth of field reads: `texels_per_pixel` of it per picture pixel,
  // `width` x `height` texels in use. Null: no depth of field this frame.
  struct Depth {
    nvrhi::ITexture* texture = nullptr;
    float texels_per_pixel_x = 1, texels_per_pixel_y = 1;
    uint32_t width = 0, height = 0;
  };
  // Into `target` (a render target, `width` x `height` from its top left). False when it cannot be done.
  bool Apply(nvrhi::ICommandList* cl, nvrhi::ITexture* target, uint32_t width, uint32_t height,
             const NrPhotoSettings& s, const Depth& depth);
  uint64_t applied() const { return applied_; }

 private:
  struct Constants;
  nvrhi::IGraphicsPipeline* Pipeline(const nvrhi::FramebufferInfo& info);

  nvrhi::IDevice* device_;
  bool ready_ = false;
  uint64_t applied_ = 0;
  nvrhi::ShaderHandle vs_, ps_;
  nvrhi::BindingLayoutHandle layout_;
  nvrhi::BufferHandle constants_;
  nvrhi::TextureHandle copy_, no_depth_;
  std::map<uint32_t, nvrhi::GraphicsPipelineHandle> pipelines_;
};

}  // namespace nr
