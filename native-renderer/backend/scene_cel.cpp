// The game renderer's cel shading: see scene_cel.h.

#include "backend/scene_cel.h"

#include <algorithm>
#include <cmath>

#include "backend/log.h"

namespace nr {

SceneCel::SceneCel(nvrhi::IDevice* device, const CelConfig& config, std::vector<AoRule> rules)
    : device_(device), config_(config), rules_(std::move(rules)) {}

SceneCel::~SceneCel() = default;

bool SceneCel::Initialize() {
  gtao_ = std::make_unique<Gtao>(device_);
  cel_ = std::make_unique<CelShade>(device_);
  if (!gtao_->Initialize() || !cel_->Initialize()) {
    gtao_.reset();
    cel_.reset();
    return false;
  }
  Logf(LogLevel::kInfo, "rexgpu-native: cel shading: outline %.2f px at 1080 lines, darkness %.2f, %u bands, strength %.2f%s",
       config_.outline, config_.darkness, config_.bands, config_.strength, config_.debug ? " (lines view)" : "");
  return true;
}

void SceneCel::NewFrame(uint64_t frame) {
  frame_ = frame;
  applied_ = false;
  ++stats_.frames;
  if (stats_.frames % 600 == 0) {
    Logf(LogLevel::kInfo,
         "rexgpu-native: cel shading last 600 frames: %llu at the opaque pass's end (%llu with the AO's normals); %llu "
         "moments without a camera, %llu without this frame's depth",
         static_cast<unsigned long long>(stats_.applied - logged_.applied),
         static_cast<unsigned long long>(stats_.ao_normals - logged_.ao_normals),
         static_cast<unsigned long long>(stats_.no_camera - logged_.no_camera),
         static_cast<unsigned long long>(stats_.no_depth - logged_.no_depth));
    logged_ = stats_;
  }
}

bool SceneCel::Apply(nvrhi::ICommandList* cl, const Gtao* ao, bool depth_before_draw) {
  if (!depth_before_draw || !tracker_.depth_valid() || !scene_depth_.texture || !scene_color_.texture) {
    ++stats_.no_depth;
    return false;
  }
  nvrhi::ITexture* depth = scene_depth_.texture;
  // The AO's normals when they are smoothed as our own (at the AO's resolution: half above render scale
  // 1.5, as our own would be).
  const bool reuse = ao && ao->normals_smoothed() && ao->normals();
  const Gtao* source = reuse ? ao : gtao_.get();
  if (timer_) timer_->Begin(cl);
  bool ok = true;
  if (!reuse) {
    AoProjection p;
    AoScreen sc;
    if (!camera_.Get(frame_, p, sc)) {
      if (timer_) timer_->End(cl);
      ++stats_.no_camera;
      if (!warned_camera_) {
        Logf(LogLevel::kWarning, "rexgpu-native: cel shading: no camera found among the scene's draws yet; none until "
                                 "there is one");
        warned_camera_ = true;
      }
      return false;
    }
    const nvrhi::TextureDesc& dd = depth->getDesc();
    const int32_t x0 = std::max(0, int32_t(std::lround(sc.left() * sc.host_scale_x)));
    const int32_t y0 = std::max(0, int32_t(std::lround(sc.top() * sc.host_scale_y)));
    const int32_t x1 = std::min(int32_t(dd.width), int32_t(std::lround((sc.left() + sc.width()) * sc.host_scale_x)));
    const int32_t y1 = std::min(int32_t(dd.height), int32_t(std::lround((sc.top() + sc.height()) * sc.host_scale_y)));
    if (x1 - x0 < 16 || y1 - y0 < 16) {
      if (timer_) timer_->End(cl);
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
    GtaoSettings gs;
    gs.half = sc.host_scale_x > 1.5f || sc.host_scale_y > 1.5f;  // as the AO's "auto" (4K: half)
    gs.strength = 2;          // the smoothing radius follows the AO's medium radius
    gs.normal_smoothing = 2;  // High: low-poly facets are not creases
    ok = gtao_->Prepare(cl, in, gs);
    if (ok) gtao_->SmoothNormals(cl, gs);
  }
  ok = ok && cel_->Apply(cl, *source, scene_color_.texture, depth, config_, frame_);
  if (timer_) timer_->End(cl);
  if (!ok) return false;
  if (reuse) ++stats_.ao_normals;
  return true;
}

void SceneCel::OnDraw(nvrhi::ICommandList* cl, const SceneAo::Draw& d, const Gtao* ao) {
  last_applied_ = false;
  if (!cel_) return;
  if (d.frame != frame_) NewFrame(d.frame);
  const AoRule* matched = nullptr;
  const AoDrawClass cls = rules_.Classify(d.state, &matched);
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
  // OnDraw records a future write; this hook shades before that draw executes.
  const bool depth_before_draw = tracker_.frame() == d.frame && tracker_.depth_valid();
  const AoFrameTracker::Events e = tracker_.OnDraw(td);
  if (e.in_scene) {
    if (d.color.key && d.color.key == tracker_.scene_color()) scene_color_ = d.color;
    if (d.depth.key && d.depth.key == tracker_.scene_depth()) scene_depth_ = d.depth;
  }
  if (e.opaque_complete && !applied_ && config_.any() && Apply(cl, ao, depth_before_draw)) {
    applied_ = last_applied_ = true;
    if (stats_.applied++ == 0)
      Logf(LogLevel::kInfo, "rexgpu-native: cel shading: first frame at the end of the opaque pass (frame %llu, %u opaque "
                            "draws, %s normals)",
           static_cast<unsigned long long>(frame_), tracker_.opaque_draws(), stats_.ao_normals ? "the AO's" : "its own");
  }
}

void SceneCel::OnDepthClear(uint64_t frame, uint64_t depth_key) {
  if (!cel_) return;
  if (frame != frame_) NewFrame(frame);
  tracker_.OnDepthCleared(frame, depth_key);
}

}  // namespace nr
