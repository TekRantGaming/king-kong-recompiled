#include "backend/upscale_frame.h"

#include <cstdlib>
#include <sstream>

namespace nr {

// King Kong's post chain (docs/upscalers.md, "Where it runs"): the AfterEffects pixel shaders other than
// the two fog passes (which end the opaque pass and belong to the scene), and the rain's downsample and
// mist composite (psrain 19408DAF... also draws masks before the lit pass, where the point cannot come).
// Hashes from the shader database (kkshaders ao-list --family psaftereffects / psrain).
const char kDefaultUpscaleRules[] =
    "post ps=C4887065264F71EA\n"
    "post ps=8316EDE09C2EF616\n"
    "post ps=F6ACAF1741114232\n"
    "post ps=97230F8492C76E78\n"
    "post ps=3C5A9EFF5A0F1E68\n"
    "post ps=5915C46F6EF0F08D\n"
    "post ps=52D6C130FDCFA4CC\n"
    "post ps=EC85BA15F4E9DC06\n"
    "post ps=2A841607AB386FCF\n"
    "post ps=B19B15ADE267E6C5\n"
    "post ps=F1C24E109CB67537\n"
    "post ps=FE14445AE8F6FCA4\n"
    "post ps=FCF2AB08E3740514\n"
    "post ps=30A0FF0EB28905F8\n"
    "post ps=9427420544F9F371\n"
    "post ps=085AB7699509AFD2\n"
    "post ps=199FFBAEA54F3AA1\n"
    "post ps=7137EA1754A79036\n"
    "post ps=82B90CB97AE23E92\n"
    "post ps=19408DAF3EB827D7\n"
    "post ps=8A2FD92C1ECE4873\n";

UpscaleRules::UpscaleRules() { AddFile(kDefaultUpscaleRules); }

void UpscaleRules::AddFile(const std::string& text, std::vector<std::string>* errors) {
  std::istringstream in(text);
  std::string line;
  int number = 0;
  while (std::getline(in, line)) {
    ++number;
    if (const size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
    std::istringstream words(line);
    std::string kind, item;
    if (!(words >> kind)) continue;
    bool ok = kind == "post" || kind == "scene";
    uint64_t ps = 0;
    while (ok && (words >> item)) {
      if (item.rfind("ps=", 0) == 0) {
        char* end = nullptr;
        ps = std::strtoull(item.c_str() + 3, &end, 16);
        ok = end && *end == 0 && ps != 0;
      } else {
        ok = false;
      }
    }
    if (!ok || !ps) {
      if (errors) errors->push_back("line " + std::to_string(number) + ": " + line);
      continue;
    }
    (kind == "post" ? post_ : scene_).insert(ps);
    (kind == "post" ? scene_ : post_).erase(ps);
  }
}

const char* UpscaleFrameTracker::TriggerName(Trigger t) {
  switch (t) {
    case Trigger::kNone: return "none";
    case Trigger::kPostShader: return "post shader";
    case Trigger::kPartialTarget: return "part of the scene target";
    case Trigger::kResolveClear: return "resolve with clear";
    case Trigger::kPresent: return "Present";
  }
  return "?";
}

UpscaleFrameTracker::UpscaleFrameTracker(UpscalePoint point) : point_(point) {}

void UpscaleFrameTracker::NewFrame(uint64_t frame) {
  if (frame_ != UINT64_MAX) previous_scene_ = scene_seen_;
  frame_ = frame;
  scene_seen_ = false;
  opaque_ended_ = false;
  // A frame starts in the scene phase when the last one had a scene (or was the first): a menu or a
  // movie frame is drawn at the output size from the start.
  phase_ = previous_scene_ ? Phase::kScene : Phase::k2D;
  ++stats_.frames;
}

bool UpscaleFrameTracker::PointAllowed() const {
  return point_ == UpscalePoint::kPost && phase_ == Phase::kScene && scene_seen_ && opaque_ended_;
}

void UpscaleFrameTracker::MarkUpscaled() {
  if (phase_ == Phase::kScene) phase_ = Phase::kPost;
}

UpscaleFrameTracker::DrawResult UpscaleFrameTracker::OnDraw(const Draw& d) {
  if (d.frame != frame_) NewFrame(d.frame);
  DrawResult r;
  const AoRuleTable& table = classifier_ ? *classifier_ : default_classifier_;
  r.cls = table.Classify(d.state);
  AoFrameTracker::Draw td;
  td.frame = d.frame;
  td.cls = r.cls;
  td.color_key = d.color_key;
  td.depth_key = d.depth_key;
  td.frame_size = d.frame_size;
  td.writes_depth = d.state.depth_write;
  // After the upscale point the targets are the output generation: the scene tracker is done.
  AoFrameTracker::Events e;
  if (phase_ == Phase::kScene || phase_ == Phase::k2D) e = tracker_.OnDraw(td);
  r.in_scene = e.in_scene;
  if (e.in_scene) scene_seen_ = true;
  if (e.opaque_complete) {
    r.opaque_complete = true;
    opaque_ended_ = true;
  }
  if (PointAllowed()) {
    const bool scene_target = d.color_key && d.color_key == tracker_.scene_color();
    if (rules_ && rules_->IsPost(d.state.ps_hash)) {
      r.trigger = Trigger::kPostShader;
    } else if (scene_target && !d.covers_target) {
      r.trigger = Trigger::kPartialTarget;
    }
    if (r.trigger != Trigger::kNone) {
      r.upscale_before = true;
      ++stats_.by_trigger[uint32_t(r.trigger)];
    }
  }
  r.phase = r.upscale_before ? Phase::kPost : phase_;
  r.jitter = r.phase == Phase::kScene && d.frame_size && d.state.depth_test;
  r.motion = r.phase == Phase::kScene && r.in_scene && d.state.depth_write && d.depth_key &&
             d.depth_key == tracker_.scene_depth();
  return r;
}

bool UpscaleFrameTracker::OnClear(const Clear& c, Trigger* trigger) {
  if (c.frame != frame_) NewFrame(c.frame);
  if (c.depth_key) tracker_.OnDepthCleared(c.frame, c.depth_key);
  if (!PointAllowed() || c.covers_target || !c.color_key || c.color_key != tracker_.scene_color()) return false;
  if (trigger) *trigger = Trigger::kPartialTarget;
  ++stats_.by_trigger[uint32_t(Trigger::kPartialTarget)];
  return true;
}

bool UpscaleFrameTracker::OnResolve(const Resolve& r, Trigger* trigger) {
  if (r.frame != frame_) NewFrame(r.frame);
  if (!PointAllowed() || !r.clears_color || !r.source_key || r.source_key != tracker_.scene_color()) return false;
  if (trigger) *trigger = Trigger::kResolveClear;
  ++stats_.by_trigger[uint32_t(Trigger::kResolveClear)];
  return true;
}

bool UpscaleFrameTracker::OnPresent(uint64_t frame) {
  if (frame != frame_) NewFrame(frame);
  if (scene_seen_) ++stats_.scene_frames;
  if (phase_ == Phase::k2D) ++stats_.frames_2d;
  const bool upscale = phase_ == Phase::kScene && scene_seen_;
  if (upscale) {
    ++stats_.by_trigger[uint32_t(Trigger::kPresent)];
    phase_ = Phase::kPost;
  }
  return upscale;
}

}  // namespace nr
