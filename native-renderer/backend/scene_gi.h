// The game renderer's screen-space global illumination (docs/gi.md): when in the frame the bounce
// light is gathered and where it goes.
//
//  - Every draw is classified and followed through the frame with the ambient occlusion's rule
//    table and tracker (ao_classifier.h; its own copies, so neither changes the other), and the
//    camera is voted from the scene draws' world-view-projection constants (ao_camera.h).
//  - At the end of the scene's opaque pass (in King Kong: just before its fog pass), the depth is
//    prepared as XeGTAO prepares it (gtao.h), the bounce light gathered from the lit scene colour
//    (ssgi.h) and composited into the scene colour target. Water, fur, smoke, fire, rain, the sky
//    and the HUD are drawn after it and never touched.
//  - A frame the rules do not cut gets it at Present, when the scene target still holds the scene.
//  - The frame's flat ambient light (the lighting shaders' g_vAmbientColor) is read from the
//    constants of the scene's draws; the composite needs it to tell surface colour from light.
//
// With the ambient occlusion on (either method), the GI adds light only: the AO darkens.
//
// The game renderer calls it from small hooks (game_gi.cpp). NVRHI only; the render thread.
#pragma once

#include <cstdint>
#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "backend/ao_camera.h"
#include "backend/ao_classifier.h"
#include "backend/ao_timer.h"
#include "backend/gi_config.h"
#include "backend/gtao.h"
#include "backend/scene_ao.h"
#include "backend/ssgi.h"
#include "backend/upscale_motion.h"

namespace nr {

class SceneGi {
 public:
  // rules: classifier rules before the built-in ones (--native_ao_rules).
  SceneGi(nvrhi::IDevice* device, const GiConfig& config, std::vector<AoRule> rules);
  ~SceneGi();
  bool Initialize();
  const GiConfig& config() const { return config_; }
  void SetTimer(AoGpuTimer* timer) { timer_ = timer; }

  // A draw as the ambient occlusion describes it (scene_ao.h). Before it is recorded: classify,
  // follow, and at the end of the opaque pass add the bounce light into `cl`, before the draw.
  // ao_on: the ambient occlusion setting is on this frame (the GI then adds light only).
  // motion_key: the draw's identity across frames (its shaders, buffers and range; 0: none), for the
  // camera's reprojection of the temporal accumulation.
  void OnDraw(nvrhi::ICommandList* cl, const SceneAo::Draw& d, bool ao_on, uint64_t motion_key = 0);
  // The flat ambient light a lighting shader of the scene was drawn with (rgb, the game's units).
  void NoteAmbient(uint64_t frame, const float rgb[3]);
  // A clear of a depth target (Clear with Z, or a resolve's depth clear).
  void OnDepthClear(uint64_t frame, uint64_t depth_key);
  // A copy of a colour target (a resolve): a copy of the scene after its opaque pass began means
  // Present no longer holds the scene alone.
  void OnColorCopy(uint64_t frame, uint64_t source_key);
  // At Present, before the back buffer is shown.
  void OnPresent(nvrhi::ICommandList* cl, bool ao_on);

  struct Stats {
    uint64_t frames = 0;           // frames with the GI on
    uint64_t applied_opaque = 0;   // at the end of the opaque pass
    uint64_t applied_present = 0;  // at Present
    uint64_t no_camera = 0;        // a moment came without a camera
    uint64_t no_depth = 0;         // or without this frame's depth
    uint64_t ambient_frames = 0;   // frames whose ambient light was read from their draws
    uint64_t history_frames = 0;   // moments that blended in last frame's light (the camera was found)
  };
  const Stats& stats() const { return stats_; }
  // What the last OnDraw did (--native_dump_frame).
  bool last_applied() const { return last_applied_; }
  // The ambient light the last moment used (tests, logging).
  const float* ambient() const { return ambient_used_; }
  const Ssgi* ssgi() const { return ssgi_.get(); }

 private:
  void NewFrame(uint64_t frame);
  // The camera's reprojection from last frame (temporal accumulation): the scene draws met again vote.
  void NoteCamera(uint64_t key, const float rows[16]);
  bool Reprojection(float out[16]);
  bool Apply(nvrhi::ICommandList* cl, bool ao_on);

  nvrhi::IDevice* device_;
  GiConfig config_;
  AoRuleTable rules_;
  AoFrameTracker tracker_;
  AoCameraFinder camera_;
  std::unique_ptr<Gtao> gtao_;
  std::unique_ptr<Ssgi> ssgi_;
  AoGpuTimer* timer_ = nullptr;
  Stats stats_, logged_;

  uint64_t frame_ = UINT64_MAX;
  bool applied_ = false;
  bool scene_copied_ = false;
  bool last_applied_ = false;
  SceneAo::Target scene_color_, scene_depth_;
  // The ambient light: the sum and count of this frame's readings, and the last frame's mean.
  double ambient_sum_[3] = {};
  uint32_t ambient_count_ = 0;
  bool have_ambient_ = false;
  float ambient_last_[3] = {0.25f, 0.25f, 0.25f};
  float ambient_used_[3] = {0.25f, 0.25f, 0.25f};
  bool warned_camera_ = false;
  // Each scene draw's world-view-projection rows by its identity, this frame and last frame (in draw
  // order for draws with the same identity).
  std::unordered_map<uint64_t, std::vector<std::array<float, 16>>> rows_now_, rows_before_;
  size_t rows_count_ = 0;
  CameraReprojectionVote vote_;
  bool applied_before_ = false;  // last frame had the bounce light (its history is this frame's)
};

}  // namespace nr
