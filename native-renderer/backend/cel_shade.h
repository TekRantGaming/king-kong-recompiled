// Cel shading's pass (docs/cel.md, shaders/cel.hlsl): ink lines and flat colour bands over the
// picture a Gtao::Prepare last prepared, into the scene colour target from a copy of it (alpha
// untouched, the sky left alone). Its own Gtao gives the normals when the AO's are not there.
//
// Independent of the game renderer (NVRHI only), recorded on the render thread.
#pragma once

#include <cstdint>
#include <map>
#include <tuple>

#include <nvrhi/nvrhi.h>

#include "backend/cel_config.h"
#include "backend/gtao.h"

namespace nr {

class CelShade {
 public:
  explicit CelShade(nvrhi::IDevice* device);
  ~CelShade();

  bool Initialize();
  bool ready() const { return ready_; }

  // Into `color` over the picture `gtao` last prepared: lines from `depth` (the scene's depth target,
  // host pixels) and the gtao's normals, bands per `c`. False when it cannot be done.
  bool Apply(nvrhi::ICommandList* cl, const Gtao& gtao, nvrhi::ITexture* color, nvrhi::ITexture* depth,
             const CelConfig& c);

  struct Stats {
    uint64_t applied = 0;
  };
  const Stats& stats() const { return stats_; }
  // Submitted command lists own the views they use until GPU completion.
  void ForgetTargetViews() { sets_.clear(); framebuffers_.clear(); }

 private:
  struct Constants;
  nvrhi::IFramebuffer* Framebuffer(nvrhi::ITexture* color);
  nvrhi::IGraphicsPipeline* GraphicsPipeline(const nvrhi::FramebufferInfo& info);

  nvrhi::IDevice* device_;
  bool ready_ = false;
  Stats stats_;
  nvrhi::ShaderHandle vs_, ps_;
  nvrhi::BindingLayoutHandle layout_;
  nvrhi::BufferHandle constants_;
  nvrhi::TextureHandle copy_;
  std::map<std::tuple<nvrhi::ITexture*, nvrhi::ITexture*>, nvrhi::BindingSetHandle> sets_;
  std::map<nvrhi::ITexture*, nvrhi::FramebufferHandle> framebuffers_;
  std::map<uint32_t, nvrhi::GraphicsPipelineHandle> pipelines_;
};

}  // namespace nr
