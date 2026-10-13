// The game renderer's ambient occlusion with --native_ao_method=gtao (docs/ao.md): when in
// the frame the occlusion is worked out and where it goes.
//
//  - Every draw is classified (ao_classifier.h) and followed through the frame; the camera is
//    voted from the scene draws' world-view-projection constants (ao_camera.h).
//  - apply=opaque: at the end of the scene's opaque pass, just before its first transparent or
//    effect draw, XeGTAO (gtao.h) runs on the scene's depth and the scene's colour target is
//    multiplied by it in place. Water, glass, smoke, particles and fog then draw over it unaffected.
//  - apply=ambient: at the end of the depth pre-pass the occlusion is worked out once; the lighting
//    pixel shaders of the list read it (the translator's ambient hook) and multiply their ambient
//    term. With no pre-pass, no listed shader drawn, or ao_mode 2, it falls back to opaque.
//  - apply=resolve: today's moment (the full-screen depth resolve, then the scene copy), with
//    GTAO instead of the legacy occlusion; the copy gets it, under the game's fog.
//  - Fallbacks, for frames the rules do not cut: the scene copy (as resolve), else the Present
//    (when the scene target still holds the scene).
//
// The game renderer calls it from small hooks (game_ao.cpp). NVRHI only; the render thread.
#pragma once

#include <cstdint>
#include <memory>

#include <nvrhi/nvrhi.h>

#include "backend/ambient_occlusion.h"
#include "backend/ao_camera.h"
#include "backend/ao_classifier.h"
#include "backend/ao_config.h"
#include "backend/ao_timer.h"
#include "backend/gtao.h"
#include "kkshaders/ambient.h"

namespace nr {

class SceneAo {
 public:
  SceneAo(nvrhi::IDevice* device, const AoConfig& config);
  ~SceneAo();
  bool Initialize();
  const AoConfig& config() const { return config_; }
  // --native_ao_gpu_time: times every piece of GTAO work (null: not timed).
  void SetTimer(AoGpuTimer* timer) { timer_ = timer; }
  // The ambient-only list (null unless apply=ambient).
  std::shared_ptr<const kkshaders::AmbientTable> ambient_table() const { return ambient_; }

  // A render or depth target as the game renderer has it.
  struct Target {
    uint64_t key = 0;  // 0: none
    nvrhi::ITexture* texture = nullptr;
    float scale_x = 1.0f, scale_y = 1.0f;
  };
  struct Draw {
    uint64_t frame = 0;
    AoDrawState state;
    Target color;  // render target 0
    Target depth;
    bool frame_size = false;               // the targets have the frame's pitch
    const uint8_t* vs_constants = nullptr;  // the device's vertex constants (big-endian, 256 x 16 bytes)
    AoScreen screen;                        // the draw's viewport, with its target's render scale
    MotionDrawKey motion_key;
  };
  // Before a draw is recorded: classify, follow, and at the moments above work the occlusion out and
  // apply it (into `cl`, before the draw). True when the draw goes to the scene after the ambient-only
  // occlusion is there (its pixel shaders may read it: ambient_view()).
  bool OnDraw(nvrhi::ICommandList* cl, const Draw& d, const AoSettings& s);
  // The game's fog pass (the AfterEffects fog shaders' c0..c3), for the resolve moment.
  void NoteFog(const float constants[16], uint64_t frame);
  // A clear of a depth target (Clear with Z, or a resolve's depth clear).
  void OnDepthClear(uint64_t frame, uint64_t depth_key);

  struct Resolve {
    AoResolve facts;     // the legacy detection's facts (ambient_occlusion.h)
    Target source;       // the resolve's source target
    bool origin = false; // the rectangle starts at the target's top left
    bool clears_depth = false;
    uint64_t depth_key = 0;  // the depth target the resolve clears
  };
  // Returns the texture the resolve should read instead of the source (the scene copy with the
  // occlusion), or null.
  nvrhi::ITexture* OnResolve(nvrhi::ICommandList* cl, const Resolve& r, const AoSettings& s);
  // At Present, before the back buffer is shown.
  void OnPresent(nvrhi::ICommandList* cl, const AoSettings& s);

  // Ambient-only: the occlusion the listed shaders read this frame (null until it is worked out,
  // and outside the scene's targets), where its picture starts in the target, and its size.
  struct AmbientView {
    nvrhi::ITexture* texture = nullptr;
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
  };
  AmbientView ambient_view() const { return ambient_view_; }
  // Whether a pixel shader is on the ambient list.
  bool IsAmbientShader(uint64_t ucode_hash) const { return ambient_ && ambient_->find(ucode_hash); }

  // What the last OnDraw decided (--native_dump_frame logs it).
  struct LastDraw {
    AoDrawClass cls = AoDrawClass::kIgnore;
    AoFrameTracker::Events events;
    bool computed = false;  // the occlusion was worked out before this draw
    bool applied = false;   // and went into the scene before it
  };
  const LastDraw& last_draw() const { return last_; }

  struct Stats {
    uint64_t frames = 0;             // frames with AO on
    uint64_t computed = 0;           // XeGTAO runs
    uint64_t applied_opaque = 0;     // into the scene before its first transparent or effect draw
    uint64_t applied_resolve = 0;    // into a scene copy
    uint64_t applied_present = 0;    // into the scene at Present
    uint64_t ambient_frames = 0;     // frames whose listed shaders read it
    uint64_t no_camera = 0;          // a moment came without a camera
    uint64_t no_depth = 0;           // a moment came without this frame's depth
  };
  const Stats& stats() const { return stats_; }
  // The classifier and tracker (tests, logging).
  const AoRuleTable& rules() const { return rules_; }
  const AoFrameTracker& tracker() const { return tracker_; }
  const Gtao* gtao() const { return gtao_.get(); }

 private:
  void NewFrame(uint64_t frame);
  // Works the occlusion out for this frame (once) from the scene's depth target if it still holds
  // the frame's depth, else from the depth resolve's copy. False when there is nothing to work from.
  bool Compute(nvrhi::ICommandList* cl, const AoSettings& s, nvrhi::ITexture* depth_override = nullptr);
  GtaoSettings Settings(const AoSettings& s) const;
  // The scene's colour target times the occlusion (timed).
  bool ApplyToTarget(nvrhi::ICommandList* cl, const AoSettings& s);
  // A resolve of the whole picture: the legacy rule (at least 1280 x 720 from the top left), or one
  // from the top left that covers the camera's picture.
  bool FullScreen(const Resolve& r) const;

  nvrhi::IDevice* device_;
  AoGpuTimer* timer_ = nullptr;
  void TimeBegin(nvrhi::ICommandList* cl) { if (timer_) timer_->Begin(cl); }
  void TimeEnd(nvrhi::ICommandList* cl) { if (timer_) timer_->End(cl); }
  AoConfig config_;
  AoRuleTable rules_;
  std::shared_ptr<const kkshaders::AmbientTable> ambient_;
  std::unique_ptr<Gtao> gtao_;
  AoFrameTracker tracker_;
  AoCameraFinder camera_;
  MotionHistory motion_history_;
  CameraReprojectionVote motion_vote_;
  Stats stats_;

  uint64_t frame_ = UINT64_MAX;
  bool computed_ = false;        // this frame
  bool applied_ = false;         // the scene has its occlusion this frame (or a copy got it)
  bool ambient_ready_ = false;   // the listed shaders can read it this frame
  bool ambient_drawn_ = false;   // a listed shader drew in the scene after that
  bool scene_resolved_ = false;  // the scene target was copied after its opaque pass began
  Target scene_color_, scene_depth_;
  nvrhi::ITexture* depth_copy_ = nullptr;  // this frame's full-screen depth resolve, kept
  bool depth_copied_ = false;              // this frame
  uint32_t scene_dest_ = 0;                // the resolve moment: the scene copy's destination
  bool scene_dest_set_ = false;
  bool resolve_applied_ = false;           // a scene copy got it this frame
  AoFog fog_;
  uint64_t fog_frame_ = UINT64_MAX;
  AmbientView ambient_view_;
  bool warned_camera_ = false;
  const char* failure_reason_ = "no apply point";
  LastDraw last_;
  Stats logged_;  // the statistics at the last periodic log line
};

}  // namespace nr
