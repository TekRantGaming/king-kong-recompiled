// The game renderer's cel shading (docs/cel.md): when in the frame it runs.
//
//  - Every draw is classified and followed through the frame with the ambient occlusion's rule
//    table and tracker (ao_classifier.h; its own copies), and the camera is voted from the scene
//    draws' world-view-projection constants (ao_camera.h), as the GI does (scene_gi.h).
//  - At the end of the scene's opaque pass (in King Kong: just before its fog pass), after the AO
//    and the GI went in, the lines and bands go into the scene colour target (cel_shade.h). Water,
//    smoke, fire, particles, fog, the light shafts and the HUD are drawn after it and never touched;
//    the sky (cleared depth) is skipped. At the scene's render resolution, before any upscaler.
//  - The normals: the AO's when it worked them out this frame with its normal smoothing on; else a
//    Gtao::Prepare of its own (smoothed High; half resolution above render scale 1.5, as the AO's
//    "auto"). The depth lines always use the scene's own depth at host pixels.
//  - A frame the rules do not cut gets none (no fallback at Present: the HUD may be in the target).
//
// The game renderer calls it from small hooks (game_cel.cpp). NVRHI only; the render thread.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "backend/ao_camera.h"
#include "backend/ao_classifier.h"
#include "backend/ao_timer.h"
#include "backend/cel_config.h"
#include "backend/cel_shade.h"
#include "backend/gtao.h"
#include "backend/scene_ao.h"

namespace nr {

class SceneCel {
 public:
  // rules: classifier rules before the built-in ones (--native_ao_rules).
  SceneCel(nvrhi::IDevice* device, const CelConfig& config, std::vector<AoRule> rules);
  ~SceneCel();
  bool Initialize();
  const CelConfig& config() const { return config_; }
  void SetConfig(const CelConfig& config) { config_ = config; }
  void SetTimer(AoGpuTimer* timer) { timer_ = timer; }
  void ForgetTargets() {
    if (gtao_) gtao_->ForgetTargetViews();
    if (cel_) cel_->ForgetTargetViews();
    scene_color_ = {}; scene_depth_ = {};
    tracker_ = {}; camera_ = {};
    frame_ = UINT64_MAX;
    applied_ = last_applied_ = false;
  }

  // A draw as the ambient occlusion describes it (scene_ao.h). Before it is recorded: classify,
  // follow, and at the end of the opaque pass shade the scene into `cl`, before the draw.
  // ao: the AO's images when it worked them out this frame (SceneAo::prepared), else null.
  void OnDraw(nvrhi::ICommandList* cl, const SceneAo::Draw& d, const Gtao* ao = nullptr);
  // A clear of a depth target (Clear with Z, or a resolve's depth clear).
  void OnDepthClear(uint64_t frame, uint64_t depth_key);

  struct Stats {
    uint64_t frames = 0;          // frames with the cel shading on
    uint64_t applied = 0;         // at the end of the opaque pass
    uint64_t ao_normals = 0;      // of them, with the AO's normals
    uint64_t no_camera = 0;       // a moment came without a camera
    uint64_t no_depth = 0;        // or without this frame's depth
  };
  const Stats& stats() const { return stats_; }
  // What the last OnDraw did (--native_dump_frame).
  bool last_applied() const { return last_applied_; }
  const CelShade* cel() const { return cel_.get(); }

 private:
  void NewFrame(uint64_t frame);
  bool Apply(nvrhi::ICommandList* cl, const Gtao* ao, bool depth_before_draw);

  nvrhi::IDevice* device_;
  CelConfig config_;
  AoRuleTable rules_;
  AoFrameTracker tracker_;
  AoCameraFinder camera_;
  std::unique_ptr<Gtao> gtao_;
  std::unique_ptr<CelShade> cel_;
  AoGpuTimer* timer_ = nullptr;
  Stats stats_, logged_;
  uint64_t frame_ = UINT64_MAX;
  bool applied_ = false;
  bool last_applied_ = false;
  bool warned_camera_ = false;
  SceneAo::Target scene_color_, scene_depth_;
};

}  // namespace nr
