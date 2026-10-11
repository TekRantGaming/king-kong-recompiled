// The game renderer's ambient occlusion with the GTAO method: see scene_ao.h.

#include "backend/scene_ao.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "backend/log.h"

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
  Logf(LogLevel::kInfo, "rexgpu-native: AO method gtao, apply %s, quality %s, radius %s, %zu classifier rules (%zu from a file)",
       AoApplyName(config_.apply), AoQualityName(config_.quality),
       config_.radius > 0.0f ? std::to_string(config_.radius).c_str() : "from the camera", rules_.rules().size(),
       config_.rules.size());
  return true;
}

GtaoSettings SceneAo::Settings(const AoSettings& s) const {
  GtaoSettings g;
  g.mode = s.mode;
  g.strength = s.strength;
  g.half = config_.quality == AoQuality::kHalf;
  g.radius = config_.radius;
  return g;
}

void SceneAo::NewFrame(uint64_t frame) {
  frame_ = frame;
  computed_ = applied_ = ambient_ready_ = ambient_drawn_ = scene_resolved_ = false;
  depth_copied_ = scene_dest_set_ = resolve_applied_ = false;
  ambient_view_ = {};
  ++stats_.frames;
}

bool SceneAo::Compute(nvrhi::ICommandList* cl, const AoSettings& s, nvrhi::ITexture* depth_override) {
  if (computed_) return true;
  AoProjection p;
  AoScreen sc;
  if (!camera_.Get(frame_, p, sc)) {
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
  if (!depth && tracker_.depth_valid() && scene_depth_.texture) depth = scene_depth_.texture;
  if (!depth && depth_copied_) depth = depth_copy_;
  if (!depth) {
    ++stats_.no_depth;
    return false;
  }
  // The picture: the camera's viewport, in host pixels of its target (rounded as every scaled
  // rectangle is), within the depth texture.
  const nvrhi::TextureDesc& dd = depth->getDesc();
  const int32_t x0 = std::max(0, int32_t(std::lround(sc.left() * sc.host_scale_x)));
  const int32_t y0 = std::max(0, int32_t(std::lround(sc.top() * sc.host_scale_y)));
  const int32_t x1 = std::min(int32_t(dd.width), int32_t(std::lround((sc.left() + sc.width()) * sc.host_scale_x)));
  const int32_t y1 = std::min(int32_t(dd.height), int32_t(std::lround((sc.top() + sc.height()) * sc.host_scale_y)));
  if (x1 - x0 < 2 || y1 - y0 < 2) return false;
  GtaoInput in;
  in.depth = depth;
  in.x = x0;
  in.y = y0;
  in.width = uint32_t(x1 - x0);
  in.height = uint32_t(y1 - y0);
  in.projection = p;
  in.screen = sc;
  if (!gtao_->Compute(cl, in, Settings(s))) return false;
  computed_ = true;
  ++stats_.computed;
  return true;
}

bool SceneAo::OnDraw(nvrhi::ICommandList* cl, const Draw& d, const AoSettings& s) {
  if (s.mode <= 0 || !gtao_) return false;
  if (d.frame != frame_) NewFrame(d.frame);
  const AoDrawClass cls = rules_.Classify(d.state);
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
  const AoFrameTracker::Events e = tracker_.OnDraw(td);
  if (e.in_scene) {
    if (d.color.key && d.color.key == tracker_.scene_color()) scene_color_ = d.color;
    if (d.depth.key && d.depth.key == tracker_.scene_depth()) scene_depth_ = d.depth;
  }
  const AoApply apply = config_.apply;
  // Ambient-only: worked out once the depth pre-pass is done, before the first lit draw.
  if (apply == AoApply::kAmbient && e.depth_complete && s.mode < 2 && Compute(cl, s)) {
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
    } else if (scene_color_.texture && Compute(cl, s) && gtao_->ApplyToTarget(cl, scene_color_.texture, s.mode)) {
      applied_ = true;
      if (stats_.applied_opaque++ == 0)
        Logf(LogLevel::kInfo, "rexgpu-native: AO: first occlusion at the end of the opaque pass (frame %llu, %u opaque draws)",
             static_cast<unsigned long long>(frame_), tracker_.opaque_draws());
    }
  }
  const bool reads = apply == AoApply::kAmbient && ambient_ready_ && e.in_scene;
  if (reads && d.state.ps_hash && IsAmbientShader(d.state.ps_hash)) ambient_drawn_ = true;
  return reads;
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
      depth_copy_ = gtao_->CopyDepth(cl, f.source);
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
    const bool want = config_.apply == AoApply::kResolve || !applied_ || resolve_applied_;
    if (eligible && want && (depth_copied_ || tracker_.depth_valid()) &&
        Compute(cl, s, depth_copied_ ? depth_copy_ : nullptr)) {
      const AoFog fog = fog_frame_ == frame_ ? fog_ : AoFog{};
      nvrhi::ITexture* fog_depth = depth_copied_ ? depth_copy_ : scene_depth_.texture;
      result = fog_depth ? gtao_->ApplyToCopy(cl, f.source, fog_depth, fog, s.mode) : nullptr;
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
  if (Compute(cl, s) && gtao_->ApplyToTarget(cl, scene_color_.texture, s.mode)) {
    applied_ = true;
    ++stats_.applied_present;
  }
}

}  // namespace nr
