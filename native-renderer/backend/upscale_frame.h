// Where in each frame the upscaler runs (docs/upscalers.md, "Where it runs"). No GPU: the game renderer
// tells it every draw, clear and resolve, and it answers in which phase the frame is and when the
// upscale point has come.
//
// A frame of King Kong (the frame logs of V-Rex and the Venture): the water reflection, the shadow
// maps, the depth pre-pass, the lit opaque pass, the fog pass, then water, fur, particles, rain and
// heat shimmer into the same 1280x720 surface, then a full-screen copy of it (the "scene copy" the post
// effects read), then the post chain: downsamples into 640x360 corners of the same surface, the rain
// mist, the AfterEffects passes (blur, glow, motion blur) ping-ponging through textures, a final
// composite back into the surface, a last heat shimmer, the HUD, and the copy Present shows.
//
// So a frame has three phases:
//  - kScene: from the frame's start to the upscale point. Frame-size targets are the input size; the
//    depth-tested draws are jittered.
//  - kPost: from the upscale point to Present. Frame-size targets are the output size, nothing is
//    jittered: the post effects and the HUD are drawn at the output resolution.
//  - k2D: a frame whose predecessor had no 3D scene (menus, movies, loading screens): drawn at the
//    output size from its start, no jitter, no upscaler. A scene seen in it makes the next frame kScene.
//
// The upscale point is the first of these after the scene's opaque pass ended (the AO tracker's
// moment: the fog pass, or the first transparent draw):
//  - a draw whose pixel shader starts the post chain (the AfterEffects passes other than the fog,
//    the rain's downsample and mist; UpscaleRules);
//  - a draw or clear into the scene's colour target that covers less than the whole target (the
//    post chain's downsamples into a corner of it);
//  - a resolve of the scene's colour target that also clears it.
// A frame that reaches Present with a scene but without the point upscales at Present (the HUD, if
// any, with it). With UpscalePoint::kPresent that is every scene frame.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "backend/ao_classifier.h"
#include "backend/upscale_config.h"

namespace nr {

// Pixel shaders that start the post chain, and ones that never do (a rule file's "scene" lines).
class UpscaleRules {
 public:
  UpscaleRules();  // the built-in list (King Kong's)
  // Lines "post ps=<hash>" or "scene ps=<hash>", '#' comments; before the built-in list. Unreadable
  // lines are skipped and described in `errors`.
  void AddFile(const std::string& text, std::vector<std::string>* errors = nullptr);
  bool IsPost(uint64_t ps_hash) const { return ps_hash && post_.count(ps_hash) && !scene_.count(ps_hash); }
  size_t post_count() const { return post_.size(); }

 private:
  std::unordered_set<uint64_t> post_, scene_;
};
extern const char kDefaultUpscaleRules[];

class UpscaleFrameTracker {
 public:
  enum class Phase : uint8_t { kScene, kPost, k2D };
  enum class Trigger : uint8_t { kNone, kPostShader, kPartialTarget, kResolveClear, kPresent };
  static const char* TriggerName(Trigger t);

  explicit UpscaleFrameTracker(UpscalePoint point = UpscalePoint::kPost);
  void set_rules(const UpscaleRules* rules) { rules_ = rules; }
  void set_classifier(const AoRuleTable* table) { classifier_ = table; }

  struct Draw {
    uint64_t frame = 0;
    AoDrawState state;        // as the AO classifier sees it
    uint64_t color_key = 0;   // render target 0 (0: none)
    uint64_t depth_key = 0;
    bool frame_size = false;  // the targets have the guest frame's pitch
    bool covers_target = true;  // the draw's scissor covers its whole target
  };
  struct DrawResult {
    Phase phase = Phase::kScene;    // the phase this draw is drawn in (after an upscale before it)
    bool upscale_before = false;    // the upscale point is before this draw
    Trigger trigger = Trigger::kNone;
    AoDrawClass cls = AoDrawClass::kIgnore;
    bool in_scene = false;          // draws into the scene's targets
    bool opaque_complete = false;   // the scene's opaque pass ended before this draw (once a frame)
    bool jitter = false;            // drawn jittered (kScene, frame size, depth tested)
    bool motion = false;            // a candidate for the motion pass (kScene, scene targets, writes depth)
  };
  DrawResult OnDraw(const Draw& d);

  // A clear (the game's Clear, or part of a resolve) of these targets.
  struct Clear {
    uint64_t frame = 0;
    uint64_t color_key = 0, depth_key = 0;
    bool covers_target = true;
  };
  // Whether the upscale point is before this clear (it is in the phase phase() returns afterwards).
  bool OnClear(const Clear& c, Trigger* trigger = nullptr);
  struct Resolve {
    uint64_t frame = 0;
    uint64_t source_key = 0;
    bool clears_color = false;
  };
  bool OnResolve(const Resolve& r, Trigger* trigger = nullptr);
  // At Present: whether the frame still has to be upscaled (a scene and no point yet). Ends the frame.
  bool OnPresent(uint64_t frame);
  // The frame's first call of any kind: the phase is the new frame's from here.
  void OnFrame(uint64_t frame) {
    if (frame != frame_) NewFrame(frame);
  }

  Phase phase() const { return phase_; }
  uint64_t frame() const { return frame_; }
  bool has_scene() const { return scene_seen_; }
  uint64_t scene_color() const { return tracker_.scene_color(); }
  uint64_t scene_depth() const { return tracker_.scene_depth(); }
  bool depth_valid() const { return tracker_.depth_valid(); }
  void OnDepthCleared(uint64_t frame, uint64_t depth_key) { tracker_.OnDepthCleared(frame, depth_key); }
  // The upscale point was taken by the renderer (it switches the phase to kPost).
  void MarkUpscaled();

  struct Stats {
    uint64_t frames = 0, scene_frames = 0, frames_2d = 0;
    uint64_t by_trigger[5] = {};  // per Trigger
  };
  const Stats& stats() const { return stats_; }

 private:
  void NewFrame(uint64_t frame);
  bool PointAllowed() const;

  UpscalePoint point_;
  const UpscaleRules* rules_ = nullptr;
  const AoRuleTable* classifier_ = nullptr;
  AoRuleTable default_classifier_;
  AoFrameTracker tracker_;
  uint64_t frame_ = UINT64_MAX;
  Phase phase_ = Phase::k2D;
  bool scene_seen_ = false;        // this frame drew a 3D scene
  bool previous_scene_ = false;    // the frame before did
  bool opaque_ended_ = false;      // this frame's opaque pass ended
  Stats stats_;
};

}  // namespace nr
