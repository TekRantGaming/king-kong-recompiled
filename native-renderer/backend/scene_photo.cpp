// Photo mode's depth of field depth: see scene_photo.h.

#include "backend/scene_photo.h"

#include <algorithm>
#include <cmath>

#include "backend/log.h"

namespace nr {

ScenePhotoDepth::ScenePhotoDepth(nvrhi::IDevice* device, std::vector<AoRule> rules)
    : device_(device), rules_(std::move(rules)) {}

ScenePhotoDepth::~ScenePhotoDepth() = default;

bool ScenePhotoDepth::Initialize() {
  gtao_ = std::make_unique<Gtao>(device_);
  if (!gtao_->Initialize()) {
    gtao_.reset();
    return false;
  }
  return true;
}

void ScenePhotoDepth::ForgetTargets() {
  if (gtao_) gtao_->ForgetTargetViews();
  scene_depth_ = {};
  tracker_ = {};
  camera_ = {};
  frame_ = prepared_frame_ = UINT64_MAX;
  applied_ = false;
}

void ScenePhotoDepth::NewFrame(uint64_t frame) {
  frame_ = frame;
  applied_ = false;
}

void ScenePhotoDepth::OnDraw(nvrhi::ICommandList* cl, const SceneAo::Draw& d) {
  if (!gtao_) return;
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
  const bool depth_before_draw = tracker_.frame() == d.frame && tracker_.depth_valid();
  const AoFrameTracker::Events e = tracker_.OnDraw(td);
  if (e.in_scene && d.depth.key && d.depth.key == tracker_.scene_depth()) scene_depth_ = d.depth;
  if (!e.opaque_complete || applied_ || !depth_before_draw || !scene_depth_.texture) return;
  applied_ = true;
  AoProjection p;
  AoScreen sc;
  if (!camera_.Get(frame_, p, sc)) {
    if (warned_++ == 0) Logf(LogLevel::kWarning, "rexgpu-native: photo mode: no camera among the scene's draws yet");
    return;
  }
  nvrhi::ITexture* depth = scene_depth_.texture;
  const nvrhi::TextureDesc& dd = depth->getDesc();
  const int32_t x0 = std::max(0, int32_t(std::lround(sc.left() * sc.host_scale_x)));
  const int32_t y0 = std::max(0, int32_t(std::lround(sc.top() * sc.host_scale_y)));
  const int32_t x1 = std::min(int32_t(dd.width), int32_t(std::lround((sc.left() + sc.width()) * sc.host_scale_x)));
  const int32_t y1 = std::min(int32_t(dd.height), int32_t(std::lround((sc.top() + sc.height()) * sc.host_scale_y)));
  if (x1 - x0 < 16 || y1 - y0 < 16) return;
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
  gs.half = false;
  if (!gtao_->Prepare(cl, in, gs)) return;
  prepared_frame_ = frame_;
  prepared_w_ = in.width;
  prepared_h_ = in.height;
  step_ = 1;
  if (prepared_++ == 0)
    Logf(LogLevel::kInfo, "rexgpu-native: photo mode: depth of field depth %ux%u at the end of the opaque pass", in.width,
         in.height);
}

void ScenePhotoDepth::OnDepthClear(uint64_t frame, uint64_t depth_key) {
  if (!gtao_) return;
  if (frame != frame_) NewFrame(frame);
  tracker_.OnDepthCleared(frame, depth_key);
}

PhotoGrade::Depth ScenePhotoDepth::DepthFor(uint64_t frame, uint32_t width, uint32_t height) const {
  PhotoGrade::Depth d;
  if (!gtao_ || prepared_frame_ == UINT64_MAX || frame > prepared_frame_ + 1 || !width || !height) return d;
  d.texture = gtao_->view_depth();
  if (!d.texture) return d;
  d.width = prepared_w_ / step_;
  d.height = prepared_h_ / step_;
  d.texels_per_pixel_x = float(d.width) / float(width);
  d.texels_per_pixel_y = float(d.height) / float(height);
  return d;
}

}  // namespace nr
