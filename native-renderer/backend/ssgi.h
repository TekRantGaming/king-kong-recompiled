// Screen-space global illumination (docs/gi.md): one bounce of light from the surfaces on the
// screen, on the depth and normals XeGTAO prepares (gtao.h, Gtao::Prepare), with NVRHI compute
// passes (shaders/ssgi.hlsl): the scene colour into a working image with mips, the gather (a
// visibility bitmask over GTAO's slices: the light from what hides the sky, and the share of
// the sky nothing hides), an edge-aware blur, and the composite into the scene colour target.
//
// Independent of the game renderer (NVRHI only), recorded on the render thread.
#pragma once

#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "backend/gi_config.h"
#include "backend/gtao.h"

namespace nr {

struct SsgiSettings {
  GiLevelParams level;
  float intensity = 1.0f;
  float radius = 2.0f;           // world units
  float thickness = 0.5f;        // world units
  float albedo = 0.6f;           // the largest albedo the composite assumes
  float occlusion = 1.0f;        // how much of the hidden ambient light is taken away (0: none)
  float ambient[3] = {0.25f, 0.25f, 0.25f};  // the frame's flat ambient light (the game's g_vAmbientColor)
  int debug = 0;                 // GiConfig::debug
};

class Ssgi {
 public:
  explicit Ssgi(nvrhi::IDevice* device);
  ~Ssgi();

  bool Initialize();
  bool ready() const { return ready_; }

  // The bounce light over the picture Gtao::Prepare last prepared, with `color` (the scene colour
  // target that picture lies in) as the light the surfaces send. False when it cannot be done.
  bool Compute(nvrhi::ICommandList* cl, const Gtao& gtao, nvrhi::ITexture* color, const SsgiSettings& s);
  // Into `color` over the picture: the light added (and the hidden ambient taken away), from a
  // copy of it. `depth`: the scene's depth target (the sky is left alone). Alpha is not written.
  bool Composite(nvrhi::ICommandList* cl, const Gtao& gtao, nvrhi::ITexture* color, nvrhi::ITexture* depth,
                 const SsgiSettings& s);

  struct Stats {
    uint64_t computed = 0;
    uint64_t composited = 0;
  };
  const Stats& stats() const { return stats_; }
  // The working result (rgb the bounce light, a the visibility; tests).
  nvrhi::ITexture* result() const { return result_; }

  static constexpr uint32_t kRadianceMips = 3;

 private:
  struct Constants;
  struct Pass {
    nvrhi::ShaderHandle shader;
    nvrhi::BindingLayoutHandle layout;
    nvrhi::ComputePipelineHandle pipeline;
  };
  bool MakePass(Pass& pass, int id, const char* entry, const std::vector<nvrhi::BindingLayoutItem>& items);
  bool EnsureImages(const Gtao& gtao);
  void FillConstants(Constants& k, const Gtao& gtao, const SsgiSettings& s, float denoise_step) const;
  void Dispatch(nvrhi::ICommandList* cl, Pass& pass, nvrhi::IBindingSet* set, uint32_t x, uint32_t y);
  nvrhi::BindingSetHandle Set(Pass& pass, const std::vector<nvrhi::BindingSetItem>& items);
  nvrhi::IFramebuffer* Framebuffer(nvrhi::ITexture* color);
  nvrhi::IGraphicsPipeline* GraphicsPipeline(nvrhi::IFramebuffer* fb);

  nvrhi::IDevice* device_;
  bool ready_ = false;
  Stats stats_;
  Pass radiance_pass_, radiance_down_, gather_, denoise_;
  nvrhi::ShaderHandle vs_, composite_ps_;
  nvrhi::BindingLayoutHandle composite_layout_;
  nvrhi::SamplerHandle point_clamp_, linear_clamp_;
  nvrhi::BufferHandle constants_;

  // Working images (the Gtao's padded size), the result of the last Compute, a copy of the colour.
  uint32_t padded_w_ = 0, padded_h_ = 0;
  const Gtao* images_of_ = nullptr;
  nvrhi::ITexture* gtao_depth_ = nullptr;  // the Gtao images the sets bind (remade when they change)
  nvrhi::TextureHandle radiance_, gi_a_, gi_b_, copy_;
  nvrhi::ITexture* result_ = nullptr;
  std::vector<nvrhi::BindingSetHandle> fixed_sets_;
  std::map<std::tuple<int, nvrhi::ITexture*, nvrhi::ITexture*>, nvrhi::BindingSetHandle> sets_;
  std::map<nvrhi::ITexture*, nvrhi::FramebufferHandle> framebuffers_;
  std::map<uint32_t, nvrhi::GraphicsPipelineHandle> pipelines_;
};

}  // namespace nr
