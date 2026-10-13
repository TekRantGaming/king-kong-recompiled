// The game renderer's screen-space global illumination: see scene_gi.h.

#include "backend/scene_gi.h"

#include <algorithm>
#include <cmath>

#include "backend/log.h"

namespace nr {

SceneGi::SceneGi(nvrhi::IDevice* device, const GiConfig& config, std::vector<AoRule> rules)
    : device_(device), config_(config), rules_(std::move(rules)) {}

SceneGi::~SceneGi() = default;

bool SceneGi::Initialize() {
  gtao_ = std::make_unique<Gtao>(device_);
  ssgi_ = std::make_unique<Ssgi>(device_);
  if (!gtao_->Initialize() || !ssgi_->Initialize()) {
    gtao_.reset();
    ssgi_.reset();
    return false;
  }
  const GiLevelParams p = GiParamsFor(config_.level);
  Logf(LogLevel::kInfo,
       "rexgpu-native: GI %s: %u slices of %u steps, %u blur passes, quality %s, radius %.2f, thickness %s, intensity "
       "%.2f, albedo limit %.2f, occlusion %.2f%s",
       GiLevelName(config_.level), p.slices, p.steps, p.denoise, GiQualityName(config_.quality),
       config_.radius > 0.0f ? config_.radius : p.radius,
       config_.thickness > 0.0f ? std::to_string(config_.thickness).c_str() : "a quarter of the radius", config_.intensity,
       config_.albedo, config_.occlusion, config_.debug ? " (debug view)" : "");
  return true;
}

void SceneGi::NewFrame(uint64_t frame) {
  // The ambient light of the frame that ended (the next moment uses it if its own frame has none).
  if (ambient_count_ > 0) {
    for (int i = 0; i < 3; ++i) ambient_last_[i] = float(ambient_sum_[i] / double(ambient_count_));
    have_ambient_ = true;
  }
  ambient_sum_[0] = ambient_sum_[1] = ambient_sum_[2] = 0.0;
  ambient_count_ = 0;
  // The camera: this frame's draws become last frame's (only the frame just before counts).
  if (frame == frame_ + 1) {
    rows_before_.swap(rows_now_);
  } else {
    rows_before_.clear();
  }
  rows_now_.clear();
  rows_count_ = 0;
  vote_.BeginFrame(frame);
  applied_before_ = applied_ && frame == frame_ + 1;
  frame_ = frame;
  applied_ = scene_copied_ = false;
  ++stats_.frames;
  if (stats_.frames % 600 == 0) {
    Logf(LogLevel::kInfo,
         "rexgpu-native: GI last 600 frames: %llu at the opaque pass's end, %llu at Present; %llu with their own ambient "
         "light (now %.3f %.3f %.3f); %llu with last frame's light; %llu moments without a camera, %llu without this "
         "frame's depth",
         static_cast<unsigned long long>(stats_.applied_opaque - logged_.applied_opaque),
         static_cast<unsigned long long>(stats_.applied_present - logged_.applied_present),
         static_cast<unsigned long long>(stats_.ambient_frames - logged_.ambient_frames), ambient_used_[0],
         ambient_used_[1], ambient_used_[2],
         static_cast<unsigned long long>(stats_.history_frames - logged_.history_frames),
         static_cast<unsigned long long>(stats_.no_camera - logged_.no_camera),
         static_cast<unsigned long long>(stats_.no_depth - logged_.no_depth));
    logged_ = stats_;
  }
}

void SceneGi::NoteCamera(uint64_t key, const float rows[16]) {
  if (rows_count_ >= 4096) return;
  std::vector<std::array<float, 16>>& now = rows_now_[key];
  const size_t ordinal = now.size();
  std::array<float, 16> r;
  std::copy(rows, rows + 16, r.begin());
  now.push_back(r);
  ++rows_count_;
  // The same draw last frame (the same ordinal among draws with this identity): a vote for the camera.
  if (auto it = rows_before_.find(key); it != rows_before_.end() && ordinal < it->second.size())
    vote_.Offer(rows, it->second[ordinal].data());
}

bool SceneGi::Reprojection(float out[16]) {
  Mat4 m;
  uint32_t votes = 0, offers = 0;
  // At least two draws agreeing, and most of them: else the history is not trusted.
  if (!vote_.Result(m, &votes, &offers) || votes < 2 || votes * 2 < offers) return false;
  for (int i = 0; i < 16; ++i) out[i] = float(m[i]);
  return true;
}

void SceneGi::NoteAmbient(uint64_t frame, const float rgb[3]) {
  if (frame != frame_) NewFrame(frame);
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(rgb[i])) return;
  }
  for (int i = 0; i < 3; ++i) ambient_sum_[i] += double(std::clamp(rgb[i], 0.0f, 4.0f));
  ++ambient_count_;
}

bool SceneGi::Apply(nvrhi::ICommandList* cl, bool ao_on) {
  AoProjection p;
  AoScreen sc;
  if (!camera_.Get(frame_, p, sc)) {
    ++stats_.no_camera;
    if (!warned_camera_) {
      Logf(LogLevel::kWarning, "rexgpu-native: GI: no camera found among the scene's draws yet; no bounce light until "
                               "there is one");
      warned_camera_ = true;
    }
    return false;
  }
  if (!tracker_.depth_valid() || !scene_depth_.texture || !scene_color_.texture) {
    ++stats_.no_depth;
    return false;
  }
  nvrhi::ITexture* depth = scene_depth_.texture;
  const nvrhi::TextureDesc& dd = depth->getDesc();
  const int32_t x0 = std::max(0, int32_t(std::lround(sc.left() * sc.host_scale_x)));
  const int32_t y0 = std::max(0, int32_t(std::lround(sc.top() * sc.host_scale_y)));
  const int32_t x1 = std::min(int32_t(dd.width), int32_t(std::lround((sc.left() + sc.width()) * sc.host_scale_x)));
  const int32_t y1 = std::min(int32_t(dd.height), int32_t(std::lround((sc.top() + sc.height()) * sc.host_scale_y)));
  if (x1 - x0 < 16 || y1 - y0 < 16) return false;
  GtaoInput in;
  in.depth = depth;
  in.x = x0;
  in.y = y0;
  in.width = uint32_t(x1 - x0);
  in.height = uint32_t(y1 - y0);
  in.projection = p;
  in.screen = sc;
  const GiLevelParams level = GiParamsFor(config_.level);
  GtaoSettings gs;
  gs.half = level.half;
  if (config_.quality == GiQuality::kHalf) gs.half = true;
  if (config_.quality == GiQuality::kFull) gs.half = false;
  if (config_.quality == GiQuality::kAuto && !level.half) gs.half = sc.host_scale_x > 1.5f || sc.host_scale_y > 1.5f;

  SsgiSettings ss;
  ss.level = level;
  ss.intensity = config_.intensity;
  ss.radius = config_.radius > 0.0f ? config_.radius : level.radius;
  ss.thickness = config_.thickness > 0.0f ? config_.thickness : 0.25f * ss.radius;
  ss.albedo = config_.albedo;
  ss.occlusion = ao_on ? 0.0f : config_.occlusion;
  ss.debug = config_.debug;
  // Temporal accumulation: last frame's light where the camera saw the same surface.
  ss.temporal = config_.temporal;
  if (ss.temporal) {
    ss.frame = uint32_t(frame_ % 64);
    ss.history = applied_before_ && ssgi_->has_history() && Reprojection(ss.reproject);
    ss.screen_xy[0] = sc.x_scale;
    ss.screen_xy[1] = sc.x_offset + sc.offset_x();
    ss.screen_xy[2] = sc.y_scale;
    ss.screen_xy[3] = sc.y_offset + sc.offset_y();
    ss.screen_z[0] = sc.z_scale;
    ss.screen_z[1] = sc.z_offset;
    ss.screen_z[2] = sc.host_scale_x;
    ss.screen_z[3] = sc.host_scale_y;
    if (ss.history) ++stats_.history_frames;
  }
  // The ambient light: this frame's readings, else the last frame's.
  if (ambient_count_ > 0) {
    for (int i = 0; i < 3; ++i) ambient_used_[i] = float(ambient_sum_[i] / double(ambient_count_));
    ++stats_.ambient_frames;
  } else if (have_ambient_) {
    for (int i = 0; i < 3; ++i) ambient_used_[i] = ambient_last_[i];
  }
  for (int i = 0; i < 3; ++i) ss.ambient[i] = ambient_used_[i];

  if (timer_) timer_->Begin(cl);
  const bool ok = gtao_->Prepare(cl, in, gs) && ssgi_->Compute(cl, *gtao_, scene_color_.texture, ss) &&
                  ssgi_->Composite(cl, *gtao_, scene_color_.texture, depth, ss);
  if (timer_) timer_->End(cl);
  if (!ok) return false;
  const uint64_t total = stats_.applied_opaque + stats_.applied_present;
  if (total == 0 || total % 1800 == 0) {
    Logf(LogLevel::kInfo,
         "rexgpu-native: GI camera: near %.4f, far %.1f, px %.3f py %.3f; radius %.2f, thickness %.2f world units, "
         "picture %ux%u at %d,%d (%s resolution), ambient %.3f %.3f %.3f%s",
         p.near_plane(), p.far_plane(), p.px, p.py, ss.radius, ss.thickness, in.width, in.height, in.x, in.y,
         gs.half ? "half" : "full", ss.ambient[0], ss.ambient[1], ss.ambient[2],
         ambient_count_ ? "" : (have_ambient_ ? " (last frame's)" : " (none read yet: the default)"));
  }
  return true;
}

void SceneGi::OnDraw(nvrhi::ICommandList* cl, const SceneAo::Draw& d, bool ao_on, uint64_t motion_key,
                     float jitter_x, float jitter_y) {
  last_applied_ = false;
  if (!ssgi_) return;
  if (d.frame != frame_) NewFrame(d.frame);
  const AoDrawClass cls = rules_.Classify(d.state);
  if ((cls == AoDrawClass::kOpaque || cls == AoDrawClass::kDepthOnly) && d.frame_size && d.depth.key && d.vs_constants &&
      camera_.offers(frame_) < 16) {
    float rows[16];
    AoProjection p;
    if (FindAoPerspectiveRegister(d.vs_constants, rows) >= 0 && ExtractAoProjection(rows, p)) camera_.Offer(frame_, p, d.screen);
  }
  // The camera's motion since last frame, from the scene's draws before the moment.
  if (config_.temporal && motion_key && !applied_ && (cls == AoDrawClass::kOpaque || cls == AoDrawClass::kDepthOnly) &&
      d.frame_size && d.depth.key && d.vs_constants) {
    float rows[16];
    if (FindAoPerspectiveRegister(d.vs_constants, rows) >= 0) {
      // The history contains jittered pixels. Vote with the clip transform that actually
      // rendered them, including the upscaler's input-pixel offset, so a still camera
      // reprojects between alternating jitter positions instead of smearing the light.
      const float sx = d.screen.x_scale * d.screen.host_scale_x;
      const float sy = d.screen.y_scale * d.screen.host_scale_y;
      if (sx != 0.0f && sy != 0.0f) ApplyGiJitter(rows, jitter_x / sx, jitter_y / sy);
      NoteCamera(motion_key, rows);
    }
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
  if (e.opaque_complete && !applied_ && Apply(cl, ao_on)) {
    applied_ = last_applied_ = true;
    if (stats_.applied_opaque++ == 0)
      Logf(LogLevel::kInfo, "rexgpu-native: GI: first bounce light at the end of the opaque pass (frame %llu, %u opaque draws)",
           static_cast<unsigned long long>(frame_), tracker_.opaque_draws());
  }
}

void SceneGi::OnDepthClear(uint64_t frame, uint64_t depth_key) {
  if (!ssgi_) return;
  if (frame != frame_) NewFrame(frame);
  tracker_.OnDepthCleared(frame, depth_key);
}

void SceneGi::OnColorCopy(uint64_t frame, uint64_t source_key) {
  if (!ssgi_) return;
  if (frame != frame_) NewFrame(frame);
  if (source_key && source_key == tracker_.scene_color() && tracker_.phase() >= AoFrameTracker::Phase::kOpaquePass)
    scene_copied_ = true;
}

void SceneGi::OnPresent(nvrhi::ICommandList* cl, bool ao_on) {
  if (!ssgi_ || applied_) return;
  // The scene still holds the frame (no transparent draw ended its opaque pass, no copy was taken).
  if (scene_copied_ || tracker_.phase() < AoFrameTracker::Phase::kOpaquePass || !scene_color_.texture) return;
  if (Apply(cl, ao_on)) {
    applied_ = true;
    ++stats_.applied_present;
  }
}

}  // namespace nr
