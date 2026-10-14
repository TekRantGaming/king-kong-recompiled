// The lighting options' work on the game's frame (docs/lighting.md): when in the frame each one
// happens, and the passes of its own it records.
//
//  - Every draw is classified and followed through the frame with the ambient occlusion's rules and
//    tracker (ao_classifier.h; the game's fog pass ends its opaque pass), and the camera is voted from
//    the scene draws' world-view-projection constants (ao_camera.h).
//  - Water reflections: just before the first water draw into the scene (after the fog pass, when the
//    scene holds the opaque world, its fog and the opaque objects drawn after it), the scene's depth
//    becomes a view depth pyramid and its colour a copy with mips (shaders/lighting_ssr.hlsl). The water
//    shaders translated with the reflection hook read both through the KKLighting buffer (b5).
//
// The game renderer calls it from small hooks (game_lighting.cpp). NVRHI only; the render thread.
#pragma once

#include <cstdint>
#include <array>
#include <map>
#include <memory>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "backend/ao_camera.h"
#include "backend/ao_classifier.h"
#include "backend/ao_timer.h"
#include "backend/lighting_config.h"
#include "backend/lighting_shaders.h"

namespace nr {

class SceneLighting {
 public:
  SceneLighting(nvrhi::IDevice* device, const LightingConfig& config);
  ~SceneLighting();
  bool Initialize();
  const LightingConfig& config() const { return config_; }
  void SetConfig(const LightingConfig& config) {
    config_ = config;
    if (!depth_pass_.pipeline) config_.water = WaterReflections::kOriginal;
    if (!vol_vs_ || !vol_ps_) config_.volumetrics = Volumetrics::kOff;
    if (!config_.volumetrics_on()) { vol_framebuffers_.clear(); vol_pending_ = false; }
  }
  // The renderer retires these images and their bindless slots before dropping the last reference.
  std::array<nvrhi::TextureHandle, 2> ReleaseSsrImages() {
    std::array<nvrhi::TextureHandle, 2> images = {ssr_color_, ssr_depth_};
    ssr_color_ = ssr_depth_ = nullptr;
    ssr_w_ = ssr_h_ = 0;
    ssr_ = {};
    return images;
  }

  struct Target {
    uint64_t key = 0;  // 0: none
    nvrhi::ITexture* texture = nullptr;
    float scale_x = 1.0f, scale_y = 1.0f;
  };
  struct Draw {
    uint64_t frame = 0;
    AoDrawState state;
    LightFamily family = LightFamily::kNone;
    Target color;  // render target 0
    Target depth;
    bool frame_size = false;
    const uint8_t* vs_constants = nullptr;  // big-endian, 256 x 16 bytes
    AoScreen screen;
  };
  // Before a draw is recorded (into `cl`, before the draw).
  void OnDraw(nvrhi::ICommandList* cl, const Draw& d);
  void OnDepthClear(uint64_t frame, uint64_t depth_key);
  // Close a lighting span on a family/frame boundary before the next draw's scene effects.
  void BeforeDraw(nvrhi::ICommandList* cl, uint64_t frame, LightFamily family);
  // End the final lighting span before Present's AO/GI/upscaler work.
  void EndTimedDraws(nvrhi::ICommandList* cl);

  // What the volumetric light needs from the game's own passes this frame (before their OnDraw).
  struct ShadowQuad {
    float light[16] = {};               // psshadowquad's light 0 rows (c0..c3)
    float bias = 0.0f;                  // g_vShadowParams[0].z
    nvrhi::ITexture* shadow_map = nullptr;  // fetch constant 2
    nvrhi::ITexture* depth = nullptr;       // fetch constant 0: the game's depth resolve
  };
  void NoteShadowQuad(uint64_t frame, const ShadowQuad& q);
  struct Fog {
    float color[4] = {}, params[4] = {}, normalizing[4] = {};  // the fog pass's c0, c1, c2
  };
  void NoteFog(uint64_t frame, const Fog& f);
  // The game drew its own light shafts this frame (pslightshaft's render mode).
  void NoteShafts(uint64_t frame);
  // At Present.
  void OnPresent(nvrhi::ICommandList* cl);

  // What the water shaders read this frame (valid: made for this frame).
  struct SsrView {
    bool valid = false;
    nvrhi::ITexture* color = nullptr;  // RGBA16F, color_mips mips, picture-sized
    nvrhi::ITexture* depth = nullptr;  // R32F view depth pyramid, depth_mips mips
    uint32_t color_mips = 0, depth_mips = 0;
    int32_t x = 0, y = 0;  // the picture in host pixels of the scene target
    uint32_t width = 0, height = 0;
    AoViewMapping mapping;  // over the picture (step 1)
    float near_plane = 0.0f, far_plane = 0.0f;
  };
  const SsrView& ssr() const { return ssr_; }

  struct Stats {
    uint64_t frames = 0;
    uint64_t ssr_prepared = 0;  // frames whose water had the scene to reflect
    uint64_t ssr_no_camera = 0;
    uint64_t ssr_no_scene = 0;  // water drawn, but not into the scene's targets
    uint64_t volumetric = 0;           // frames with the volumetric light
    uint64_t volumetric_shafts = 0;    // of those, with the game's own shafts
    uint64_t volumetric_no_shadow = 0; // a fog pass without a screen-space shadow before it
  };
  const Stats& stats() const { return stats_; }
  AoGpuTimer* timer(int which) const;  // 0 shadows, 1 water, 2 volumetrics (null unless gpu_time)

 private:
  void NewFrame(uint64_t frame);
  bool PrepareSsr(nvrhi::ICommandList* cl);
  struct Pass {
    nvrhi::ShaderHandle shader;
    nvrhi::BindingLayoutHandle layout;
    nvrhi::ComputePipelineHandle pipeline;
  };
  bool MakePass(Pass& pass, const void* data, size_t size, const char* entry,
                const std::vector<nvrhi::BindingLayoutItem>& items);
  bool Dispatch(nvrhi::ICommandList* cl, Pass& pass, nvrhi::IBindingSet* set, uint32_t w, uint32_t h);
  bool EnsureSsrImages(uint32_t w, uint32_t h);

  nvrhi::IDevice* device_;
  LightingConfig config_;
  AoRuleTable rules_;
  AoFrameTracker tracker_;
  AoCameraFinder camera_;
  Stats stats_, logged_;
  uint64_t frame_ = UINT64_MAX;
  Target scene_color_, scene_depth_;

  // Water reflections.
  Pass depth_pass_, depth_down_pass_, color_pass_, color_down_pass_;
  nvrhi::BufferHandle constants_;
  nvrhi::TextureHandle ssr_color_, ssr_depth_;
  uint32_t ssr_w_ = 0, ssr_h_ = 0;
  SsrView ssr_;
  bool ssr_tried_ = false;  // this frame
  uint64_t hooked_water_frame_ = 0;  // the last frame that drew a water shader with the reflection hook
  bool warned_camera_ = false;

  // Volumetric light.
  bool RunVolumetric(nvrhi::ICommandList* cl);
  nvrhi::IGraphicsPipeline* VolumetricPipeline(const nvrhi::FramebufferInfo& info);
  nvrhi::ShaderHandle vol_vs_, vol_ps_;
  nvrhi::BindingLayoutHandle vol_layout_;
  nvrhi::BufferHandle vol_constants_;
  nvrhi::SamplerHandle point_clamp_;
  std::map<nvrhi::ITexture*, nvrhi::FramebufferHandle> vol_framebuffers_;
  std::map<uint32_t, nvrhi::GraphicsPipelineHandle> vol_pipelines_;  // by colour format
  ShadowQuad shadow_quad_;
  uint64_t shadow_quad_frame_ = UINT64_MAX;
  Fog fog_;
  uint64_t fog_frame_ = UINT64_MAX;
  uint64_t shafts_frame_ = UINT64_MAX;
  bool vol_pending_ = false;  // the fog pass was drawn: the next draw starts with the volumetric light

  std::unique_ptr<AoGpuTimer> timers_[3];
  // Timing a contiguous run of shadow/water draws, including lighting preparation.
  int timing_ = -1;
};

}  // namespace nr
