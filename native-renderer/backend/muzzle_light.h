// Muzzle flashes that cast light (docs/lighting.md): the game draws a shot's flash as a sprite, which lights nothing.
// Here a shot adds a small, short lived warm point light at the muzzle, so what is near it is lit for a moment: the
// enemy jumping at you, the ground, the wall.
//
//  - The game's thread tells where a flame was made (nr_muzzle_flash, in view space, from kk/src/muzzle_light.cpp).
//  - At the end of the scene's opaque pass, as the GI (scene_gi.h), a full-screen pass (shaders/muzzle.hlsl) adds the
//    light from the scene's depth and normals. Water, fur, smoke, fire, the first-person hands and the HUD are drawn
//    after it and are not lit. It casts no shadows.
//  - A frame with no flash costs one look at the list: nothing is classified, copied or drawn.
//
// The game renderer calls it from small hooks (game_muzzle.cpp). NVRHI only; the render thread.
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "backend/ao_camera.h"
#include "backend/ao_classifier.h"
#include "backend/ao_timer.h"
#include "backend/gtao.h"
#include "backend/muzzle_config.h"
#include "backend/scene_ao.h"

namespace nr {

// Tuning (docs/lighting.md); not settings.
struct MuzzleTuning {
  static constexpr uint32_t kMaxLights = 4;
  static constexpr float kRadius = 9.0f;           // view units, metres in King Kong
  static constexpr float kFalloff = 0.25f;         // 1 / (1 + this * distance^2)
  static constexpr float kSeconds = 0.10f;         // how long a flash lasts
  static constexpr float kStrength = 2.2f;         // at its start
  static constexpr float kColour[3] = {1.0f, 0.72f, 0.42f};
  static constexpr float kDarkSurface = 0.30f, kBrightSurface = 0.85f;  // reflectance assumed (muzzle.hlsl)
  static constexpr float kWrap = 0.25f;            // how far the light wraps around a surface
  static constexpr float kHueFloor = 0.015f;
};

// The game's thread: a flame was made at this view position (x right, y up, z forward).
void NoteMuzzleFlash(float x, float y, float z);

struct MuzzleLightNow {
  float x = 0, y = 0, z = 0, strength = 0;
};
// The flashes alive now, the strongest first, at most kMaxLights.
uint32_t LiveMuzzleLights(MuzzleLightNow* out, std::chrono::steady_clock::time_point now);
// How a flash's strength falls with its age (seconds): 1 at the start, 0 at kSeconds and after.
float MuzzleFade(float age);

class SceneMuzzle {
 public:
  // rules: classifier rules before the built-in ones (--native_ao_rules).
  SceneMuzzle(nvrhi::IDevice* device, std::vector<AoRule> rules);
  ~SceneMuzzle();
  bool Initialize();
  void SetTimer(AoGpuTimer* timer) { timer_ = timer; }
  void ForgetTargets();

  // Asked for every draw and depth clear: true in a frame that began with a flash alive. Only such a frame is followed.
  bool Wanted(uint64_t frame);
  // A draw as the ambient occlusion describes it (scene_ao.h), before it is recorded: at the end of the opaque pass
  // the light goes into `cl`, before the draw. ao: the AO's images when it worked them out this frame, else null.
  void OnDraw(nvrhi::ICommandList* cl, const SceneAo::Draw& d, const Gtao* ao);
  // A clear of a depth target (Clear with Z, or a resolve's depth clear).
  void OnDepthClear(uint64_t frame, uint64_t depth_key);

  struct Stats {
    uint64_t frames = 0;   // frames with a flash
    uint64_t applied = 0;  // of them, lit at the end of the opaque pass
  };
  const Stats& stats() const { return stats_; }

 private:
  struct Constants;
  bool Apply(nvrhi::ICommandList* cl, const Gtao* ao, bool depth_before_draw);
  bool Shade(nvrhi::ICommandList* cl, const Gtao& gtao, nvrhi::ITexture* color, nvrhi::ITexture* depth,
             const MuzzleLightNow* lights, uint32_t count);
  nvrhi::IFramebuffer* Framebuffer(nvrhi::ITexture* color);
  nvrhi::IGraphicsPipeline* GraphicsPipeline(const nvrhi::FramebufferInfo& info);

  nvrhi::IDevice* device_;
  AoRuleTable rules_;
  AoFrameTracker tracker_;
  AoCameraFinder camera_;
  std::unique_ptr<Gtao> gtao_;
  AoGpuTimer* timer_ = nullptr;
  Stats stats_;
  uint64_t frame_ = UINT64_MAX;
  bool wanted_ = false, applied_ = false, warned_camera_ = false;
  SceneAo::Target scene_color_, scene_depth_;

  nvrhi::ShaderHandle vs_, ps_;
  nvrhi::BindingLayoutHandle layout_;
  nvrhi::BufferHandle constants_;
  nvrhi::TextureHandle copy_;
  std::map<std::pair<nvrhi::ITexture*, nvrhi::ITexture*>, nvrhi::BindingSetHandle> sets_;
  std::map<nvrhi::ITexture*, nvrhi::FramebufferHandle> framebuffers_;
  std::map<uint32_t, nvrhi::GraphicsPipelineHandle> pipelines_;
};

}  // namespace nr
