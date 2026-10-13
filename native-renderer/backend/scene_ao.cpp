// The game renderer's ambient occlusion with the GTAO method: see scene_ao.h.

#include "backend/scene_ao.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "backend/log.h"
#include "backend/guest_memory.h"

namespace nr {

SceneAo::SceneAo(nvrhi::IDevice* device, const AoConfig& config)
    : device_(device), config_(config), rules_(config.rules) {}

SceneAo::~SceneAo() = default;

bool SceneAo::Initialize() {
  gtao_ = std::make_unique<Gtao>(device_);
  if (!gtao_->Initialize()) {
    gtao_.reset();
    return false;
  }
  if (config_.apply == AoApply::kAmbient) {
    std::vector<std::string> errors;
    auto table = std::make_shared<kkshaders::AmbientTable>(kkshaders::parseAmbientTable(config_.ambient_shaders, &errors));
    for (const std::string& e : errors) Logf(LogLevel::kWarning, "rexgpu-native: AO shader list: %s", e.c_str());
    Logf(LogLevel::kInfo, "rexgpu-native: AO ambient-only: %zu lighting shaders listed", table->byHash.size());
    ambient_ = table;
  }
  Logf(LogLevel::kInfo,
       "rexgpu-native: AO method gtao, apply %s, quality %s, radius %s (scale %.2f), falloff %.3f, power %s, %zu "
       "classifier rules (%zu from a file)",
       AoApplyName(config_.apply), AoQualityName(config_.quality),
       config_.radius > 0.0f ? std::to_string(config_.radius).c_str() : "from the camera", config_.radius_scale,
       config_.falloff > 0.0f ? config_.falloff : 0.615f,
       config_.power > 0.0f ? std::to_string(config_.power).c_str() : "from the strength", rules_.rules().size(),
       config_.rules.size());
  return true;
}

GtaoSettings SceneAo::Settings(const AoSettings& s) const {
  GtaoSettings g;
  g.mode = s.mode;
  g.strength = s.strength;
  g.sample_quality = s.sample_quality;
  g.temporal_upscaler = s.temporal_upscaler;
  g.temporal = !s.temporal_upscaler;
  g.half = config_.quality == AoQuality::kHalf;
  g.radius = config_.radius;
  g.radius_scale = config_.radius_scale;
  g.falloff = config_.falloff;
  g.power = config_.power;
  g.fade = config_.fade;
  return g;
}

void SceneAo::NewFrame(uint64_t frame) {
  if (config_.dump_every && frame_ != UINT64_MAX) {
    const bool ambient = ambient_ready_ && ambient_drawn_;
    const char* reason = applied_ || ambient ? "applied" :
        tracker_.phase() < AoFrameTracker::Phase::kOpaquePass ? "no opaque scene" :
        tracker_.expects_end() && tracker_.phase() == AoFrameTracker::Phase::kOpaquePass ? "waiting for missing fog/end" :
        !tracker_.depth_valid() && !depth_copied_ ? "depth cleared/no depth copy" :
        scene_resolved_ ? "scene already resolved" : failure_reason_;
    Logf(LogLevel::kInfo, "rexgpu-native: AO frame %llu: apply_point=%d computed=%d applied=%d reason=%s phase=%u expects_end=%d depth=%d copied=%d resolved=%d opaque=%u depth_source=%s",
         static_cast<unsigned long long>(frame_), int(tracker_.phase() == AoFrameTracker::Phase::kAfterOpaque),
         int(computed_), int(applied_ || ambient), reason, unsigned(tracker_.phase()), int(tracker_.expects_end()),
         int(tracker_.depth_valid()), int(depth_copied_), int(scene_resolved_), tracker_.opaque_draws(), depth_source_);
  }
  frame_ = frame;
  failure_reason_ = "no apply point";
  depth_source_ = "none";
  motion_history_.BeginFrame(frame);
  motion_vote_.BeginFrame(frame);
  computed_ = applied_ = ambient_ready_ = ambient_drawn_ = scene_resolved_ = false;
  depth_copied_ = scene_dest_set_ = resolve_applied_ = false;
  ambient_view_ = {};
  ++stats_.frames;
  // Every 600 frames with AO on: where the occlusion went (the Windows checklist reads this).
  if (stats_.frames % 600 == 0) {
    Logf(LogLevel::kInfo,
         "rexgpu-native: AO last 600 frames: %llu computed, %llu at the opaque pass's end, %llu in a scene copy, %llu at "
         "Present, %llu ambient-only; %llu moments without a camera, %llu without this frame's depth",
         static_cast<unsigned long long>(stats_.computed - logged_.computed),
         static_cast<unsigned long long>(stats_.applied_opaque - logged_.applied_opaque),
         static_cast<unsigned long long>(stats_.applied_resolve - logged_.applied_resolve),
         static_cast<unsigned long long>(stats_.applied_present - logged_.applied_present),
         static_cast<unsigned long long>(stats_.ambient_frames - logged_.ambient_frames),
         static_cast<unsigned long long>(stats_.no_camera - logged_.no_camera),
         static_cast<unsigned long long>(stats_.no_depth - logged_.no_depth));
    logged_ = stats_;
  }
}

bool SceneAo::Compute(nvrhi::ICommandList* cl, const AoSettings& s, nvrhi::ITexture* depth_override,
                      bool scene_depth_available) {
  if (computed_) return true;
  AoProjection p;
  AoScreen sc;
  if (!camera_.Get(frame_, p, sc)) {
    failure_reason_ = "no camera";
    ++stats_.no_camera;
    if (!warned_camera_) {
      Logf(LogLevel::kWarning, "rexgpu-native: AO: no camera found among the scene's draws yet (no perspective matrix in "
                               "their vertex constants); no occlusion until there is one");
      warned_camera_ = true;
    }
    return false;
  }
  // The scene's depth target while it holds this frame's depth, else the depth resolve's copy.
  nvrhi::ITexture* depth = depth_override;
  if (!depth && scene_depth_available && tracker_.depth_valid() && scene_depth_.texture) depth = scene_depth_.texture;
  if (!depth && depth_copied_) depth = depth_copy_;
  if (!depth) {
    failure_reason_ = "no current depth";
    ++stats_.no_depth;
    return false;
  }
  depth_source_ = depth == depth_copy_ ? "resolve copy" : "scene target";
  // The picture: the camera's viewport, in host pixels of its target (rounded as every scaled
  // rectangle is), within the depth texture.
  const nvrhi::TextureDesc& dd = depth->getDesc();
  const int32_t x0 = std::max(0, int32_t(std::lround(sc.left() * sc.host_scale_x)));
  const int32_t y0 = std::max(0, int32_t(std::lround(sc.top() * sc.host_scale_y)));
  const int32_t x1 = std::min(int32_t(dd.width), int32_t(std::lround((sc.left() + sc.width()) * sc.host_scale_x)));
  const int32_t y1 = std::min(int32_t(dd.height), int32_t(std::lround((sc.top() + sc.height()) * sc.host_scale_y)));
  if (x1 - x0 < 2 || y1 - y0 < 2) {
    failure_reason_ = "empty camera viewport";
    return false;
  }
  GtaoInput in;
  in.depth = depth;
  in.x = x0;
  in.y = y0;
  in.width = uint32_t(x1 - x0);
  in.height = uint32_t(y1 - y0);
  in.projection = p;
  in.screen = sc;
  in.frame = frame_;
  uint32_t votes = 0, offers = 0;
  in.reprojection_valid = motion_vote_.Result(in.reprojection, &votes, &offers) && votes >= 3 && votes * 2 >= offers;
  TimeBegin(cl);
  GtaoSettings gs = Settings(s);
  if (config_.quality == AoQuality::kAuto) gs.half = sc.host_scale_x > 1.5f || sc.host_scale_y > 1.5f;
  const bool ok = gtao_->Compute(cl, in, gs) != nullptr;
  TimeEnd(cl);
  if (!ok) {
    failure_reason_ = "GTAO compute failed";
    return false;
  }
  if (config_.dump_every && frame_ % config_.dump_every == 0) {
    Logf(LogLevel::kInfo, "rexgpu-native: AO denoise frame %llu: samples=%d temporal=%s camera_votes=%u/%u history_frames=%llu",
         static_cast<unsigned long long>(frame_), s.sample_quality, s.temporal_upscaler ? "upscaler" : "local",
         votes, offers, static_cast<unsigned long long>(gtao_->stats().history_frames));
  }
  if (stats_.computed == 0 || (stats_.computed % 1800) == 0) {
    // The camera and the radius it gives (docs/ao.md, "Strength and radius").
    const GtaoSettings g = Settings(s);
    const float radius = g.radius > 0.0f ? g.radius
                                         : Gtao::RadiusInNearPlanes(s.strength) * p.near_plane() * std::max(g.radius_scale, 0.01f);
    Logf(LogLevel::kInfo,
         "rexgpu-native: AO camera: near %.4f, far %.1f, px %.3f py %.3f; radius %.4f world units (strength %d), picture %ux%u at %d,%d",
         p.near_plane(), p.far_plane(), p.px, p.py, radius, s.strength, in.width, in.height, in.x, in.y);
  }
  computed_ = true;
  ++stats_.computed;
  return true;
}

bool SceneAo::OnDraw(nvrhi::ICommandList* cl, const Draw& d, const AoSettings& s) {
  if (s.mode <= 0 || !gtao_) return false;
  if (d.frame != frame_) NewFrame(d.frame);
  const AoRule* matched = nullptr;
  const AoDrawClass cls = rules_.Classify(d.state, &matched);
  const uint64_t computed_before = stats_.computed;
  const bool applied_before = applied_;
  // The camera, from the frame's first scene draws with a depth test.
  if ((cls == AoDrawClass::kOpaque || cls == AoDrawClass::kDepthOnly) && d.frame_size && d.depth.key && d.vs_constants &&
      camera_.offers(frame_) < 16) {
    float rows[16];
    AoProjection p;
    if (FindAoPerspectiveRegister(d.vs_constants, rows) >= 0 && ExtractAoProjection(rows, p)) camera_.Offer(frame_, p, d.screen);
  }
  AoFrameTracker::Draw td;
  td.frame = d.frame;
  td.cls = cls;
  td.color_key = d.color.key;
  td.depth_key = d.depth.key;
  td.frame_size = d.frame_size;
  td.writes_depth = d.state.depth_write && d.depth.key;
  td.terminal_transparent = matched && matched->ps_hash && cls == AoDrawClass::kTransparent;
  // The tracker records the upcoming draw's depth write, but AO is computed before that
  // draw executes. A water draw must not turn cleared depth into valid scene depth yet.
  const bool depth_before_draw = tracker_.frame() == d.frame && tracker_.depth_valid();
  const AoFrameTracker::Events e = tracker_.OnDraw(td);
  // Reuse the upscalers' draw matching and camera-motion vote. Local AO history is depth
  // validated and clamped, so moving hands and newly exposed surfaces reject old shading.
  if (!s.temporal_upscaler && e.in_scene && d.vs_constants &&
      (cls == AoDrawClass::kDepthOnly || cls == AoDrawClass::kOpaque) &&
      motion_history_.current_draws() < 256) {
    float rows[16];
    const int reg = FindAoPerspectiveRegister(d.vs_constants, rows);
    if (reg >= 0) {
      VsConstants constants;
      for (size_t i = 0; i < constants.size(); ++i) constants[i] = LoadBEFloat(d.vs_constants + i * 4);
      const uint32_t ordinal = motion_history_.Record(d.motion_key, constants);
      if (const VsConstants* prev = motion_history_.FindPrevious(d.motion_key, ordinal, constants))
        motion_vote_.Offer(rows, prev->data() + reg * 4);
    }
  }
  if (e.in_scene) {
    if (d.color.key && d.color.key == tracker_.scene_color()) scene_color_ = d.color;
    if (d.depth.key && d.depth.key == tracker_.scene_depth()) scene_depth_ = d.depth;
  }
  const AoApply apply = config_.apply;
  // Ambient-only: worked out once the depth pre-pass is done, before the first lit draw.
  if (apply == AoApply::kAmbient && e.depth_complete && s.mode < 2 && Compute(cl, s, nullptr, depth_before_draw)) {
    ambient_ready_ = true;
    ambient_view_ = {gtao_->occlusion(), gtao_->origin_x(), gtao_->origin_y(), gtao_->width(), gtao_->height()};
  }
  // The end of the opaque pass: into the scene before this transparent or effect draw (unless the
  // lighting shaders already took it).
  if (e.opaque_complete && !applied_ && apply != AoApply::kResolve) {
    const bool ambient_did = apply == AoApply::kAmbient && ambient_ready_ && ambient_drawn_ && s.mode < 2;
    if (ambient_did) {
      applied_ = true;
      ++stats_.ambient_frames;
    } else if (scene_color_.texture && Compute(cl, s, nullptr, depth_before_draw) && ApplyToTarget(cl, s)) {
      applied_ = true;
      if (stats_.applied_opaque++ == 0)
        Logf(LogLevel::kInfo, "rexgpu-native: AO: first occlusion at the end of the opaque pass (frame %llu, %u opaque draws)",
             static_cast<unsigned long long>(frame_), tracker_.opaque_draws());
    }
  }
  // Only the opaque scene reads it: a listed shader drawn blended (water, smoke) keeps its ambient.
  // And only draws that do not write depth: their surfaces are in the pre-pass's depth the occlusion
  // was worked out from (the game draws foliage, characters and some walls with depth writes after
  // its pre-pass; the occlusion there would be that of whatever is behind them).
  const bool reads = apply == AoApply::kAmbient && ambient_ready_ && e.in_scene && cls == AoDrawClass::kOpaque &&
                     !d.state.depth_write;
  if (reads && d.state.ps_hash && IsAmbientShader(d.state.ps_hash)) ambient_drawn_ = true;
  last_.cls = cls;
  last_.events = e;
  last_.computed = stats_.computed != computed_before;
  last_.applied = applied_ && !applied_before;
  return reads;
}

bool SceneAo::ApplyToTarget(nvrhi::ICommandList* cl, const AoSettings& s) {
  TimeBegin(cl);
  const bool ok = gtao_->ApplyToTarget(cl, scene_color_.texture, s.mode);
  if (!ok) failure_reason_ = "target apply failed";
  TimeEnd(cl);
  return ok;
}

bool SceneAo::FullScreen(const Resolve& r) const {
  if (r.facts.full_screen) return true;
  AoProjection p;
  AoScreen sc;
  if (!r.origin || !camera_.Get(frame_, p, sc)) return false;
  return r.facts.width >= uint32_t(std::lround((sc.left() + sc.width()) * sc.host_scale_x)) &&
         r.facts.height >= uint32_t(std::lround((sc.top() + sc.height()) * sc.host_scale_y));
}

void SceneAo::NoteFog(const float c[16], uint64_t frame) {
  fog_.valid = true;
  fog_.normalizing[0] = c[2 * 4 + 0];
  fog_.normalizing[1] = c[2 * 4 + 1];
  fog_.normalizing[2] = c[2 * 4 + 2];
  fog_.near_over_distance = c[1 * 4 + 0];
  fog_.density = c[1 * 4 + 2];
  fog_.inverse_distance = c[1 * 4 + 3];
  std::memcpy(fog_.color, c, sizeof(fog_.color));
  fog_frame_ = frame;
}

void SceneAo::OnDepthClear(uint64_t frame, uint64_t depth_key) {
  if (frame != frame_) NewFrame(frame);
  tracker_.OnDepthCleared(frame, depth_key);
}

nvrhi::ITexture* SceneAo::OnResolve(nvrhi::ICommandList* cl, const Resolve& r, const AoSettings& s) {
  if (s.mode <= 0 || !gtao_) return nullptr;
  const AoResolve& f = r.facts;
  if (f.frame != frame_) NewFrame(f.frame);
  nvrhi::ITexture* result = nullptr;
  if (f.from_depth) {
    // The scene's depth is complete: keep a copy (the game may clear it right after).
    if (FullScreen(r) && f.format_ok && f.source && !applied_) {
      TimeBegin(cl);
      depth_copy_ = gtao_->CopyDepth(cl, f.source);
      TimeEnd(cl);
      depth_copied_ = depth_copy_ != nullptr;
      scene_dest_set_ = false;
    }
  } else {
    if (r.source.key && r.source.key == tracker_.scene_color() &&
        tracker_.phase() >= AoFrameTracker::Phase::kOpaquePass)
      scene_resolved_ = true;
    // The scene copy: today's moment (apply=resolve), and the others' fallback when the end of the
    // opaque pass was not seen; later copies to the same destination get it too, as today.
    const bool eligible = FullScreen(r) && f.format_ok && f.source && (!scene_dest_set_ || f.dest_key == scene_dest_);
    // Not while the frame waits for its fog pass: the game copies its half-drawn scene for the
    // light shafts before it (Call Kong, Kong to the Rescue, Chased by Kong), and the fallback would
    // put the occlusion into that copy and leave none for the fog pass's moment.
    // (Kong to the Rescue copies it right after the depth pre-pass, before the first lit draw.) A copy
    // before the opaque pass began is never the scene copy.
    const AoFrameTracker::Phase phase = tracker_.phase();
    const bool want = config_.apply == AoApply::kResolve ||
                      ((!applied_ || resolve_applied_) && phase >= AoFrameTracker::Phase::kOpaquePass &&
                       !(tracker_.expects_end() && phase < AoFrameTracker::Phase::kAfterOpaque));
    if (eligible && want && (depth_copied_ || tracker_.depth_valid()) &&
        Compute(cl, s, depth_copied_ ? depth_copy_ : nullptr)) {
      const AoFog fog = fog_frame_ == frame_ ? fog_ : AoFog{};
      nvrhi::ITexture* fog_depth = depth_copied_ ? depth_copy_ : scene_depth_.texture;
      TimeBegin(cl);
      result = fog_depth ? gtao_->ApplyToCopy(cl, f.source, fog_depth, fog, s.mode) : nullptr;
      TimeEnd(cl);
      if (result) {
        scene_dest_ = f.dest_key;
        scene_dest_set_ = applied_ = resolve_applied_ = true;
        if (stats_.applied_resolve++ == 0)
          Logf(LogLevel::kInfo, "rexgpu-native: AO: first occlusion at a scene copy (frame %llu)%s",
               static_cast<unsigned long long>(frame_), config_.apply == AoApply::kResolve ? "" : " (fallback)");
      }
    }
  }
  if (r.clears_depth) tracker_.OnDepthCleared(frame_, r.depth_key);
  return result;
}

void SceneAo::OnPresent(nvrhi::ICommandList* cl, const AoSettings& s) {
  if (s.mode <= 0 || !gtao_ || applied_ || config_.apply == AoApply::kResolve) return;
  if (config_.apply == AoApply::kAmbient && ambient_ready_ && ambient_drawn_ && s.mode < 2) return;
  // The scene still holds the frame (no transparent draw ended its opaque pass, no copy was taken).
  if (scene_resolved_ || tracker_.phase() < AoFrameTracker::Phase::kOpaquePass || !scene_color_.texture) return;
  if (Compute(cl, s) && ApplyToTarget(cl, s)) {
    applied_ = true;
    ++stats_.applied_present;
  }
}

}  // namespace nr
