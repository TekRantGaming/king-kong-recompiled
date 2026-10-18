// The game renderer's photo mode (task 208, docs/photo-mode.md): what the game sends each frame
// (plugin/native_photo.h) decides which draws are left out (Jack's floating first-person arms, the pause's
// black cover), which of the player's picture settings are replaced while it lasts (cel shading, sharpening,
// brightness, gamma), and the photo grade over the finished picture (photo_grade.h) with the depth of field's
// depth from the end of the opaque pass (scene_photo.h). Renderer only; with photo mode off nothing here
// runs or is made.

#include <cmath>

#include "backend/game_renderer.h"
#include "backend/log.h"
#include "kknr/render_targets.h"
#include "kkshaders/tessellation.h"

namespace nr {

namespace {

constexpr uint64_t kPhotoRetireFrames = 8;

}  // namespace

void GameRenderer::PhotoOverrideEffects(EffectSettings& s) {
  photo_ = PhotoSettingsNow();
  photo_read_frame_ = frame_;
  if (!photo_.active) return;
  if (photo_.cel >= 0) s.cel.enabled = photo_.cel != 0;
  if (photo_.sharpness >= 0) s.sharpness = uint32_t(photo_.sharpness);
  if (std::isfinite(photo_.brightness) || std::isfinite(photo_.gamma))
    s.picture = PictureAdjust::FromSettings(
        std::isfinite(photo_.brightness) ? double(photo_.brightness) : double(s.picture.brightness) * 50.0,
        std::isfinite(photo_.gamma) ? double(photo_.gamma) : double(s.picture.gamma));
}

bool GameRenderer::PhotoHidesDraw(const DrawCall& call, const dev::DeviceView& d, const GameShader& vs,
                                  const GameShader* ps) {
  if (photo_read_frame_ != frame_) {  // without live settings (tests), read here once a frame
    photo_ = PhotoSettingsNow();
    photo_read_frame_ = frame_;
  }
  (void)d;
  const uint32_t hide = photo_.hide;
  if (!photo_.active || !hide) return false;
  bool hidden = false;
  if ((hide & 2) && ps && PhotoIsPauseCover(vs.ucode_hash, ps->ucode_hash, call.count)) hidden = true;
  // The arms: index count first (cheap), then the geometry's own bytes; Ann's and Kong's meshes never match.
  if (!hidden && (hide & 1) && call.indexed && PhotoMeshCountCandidate(call.count))
    hidden = PhotoHidesMesh(GeometryContentHash(call), call.count);
  if (hidden) ++photo_hidden_;
  return hidden;
}

void GameRenderer::PhotoDrawHook(nvrhi::ICommandList* cl, const DrawCall& call, const dev::DeviceView& d,
                                 const std::array<HostTarget*, 4>& colors, HostTarget* depth,
                                 const PipelineRecord& record, const GameShader& vs, const GameShader* ps) {
  const SceneAo::Draw a = AoDescribeDraw(call, d, colors, depth, record, vs, ps);
  scene_photo_->OnDraw(cl, a);
}

void GameRenderer::PhotoClearHook(const ClearCall& call, const HostTarget* depth) {
  if (call.depth && depth) scene_photo_->OnDepthClear(call.frame, depth->key);
}

void GameRenderer::PhotoResolveHook(const ResolveCall& call) {
  const kknr::ResolveFlags flags{call.flags};
  if (!flags.ClearDepthStencil() || !call.depth_stencil) return;
  dev::DeviceView d(call.device ? memory_.Virtual(call.device) : nullptr);
  if (!d.valid()) return;
  const uint32_t info = d.u32(dev::kRbDepthInfo), pitch = d.u32(dev::kRbSurfaceInfo) & 0x3FFF;
  if (const HostTarget* t = FindTarget(true, info & 0xFFF, pitch, (info >> 16) & 1)) scene_photo_->OnDepthClear(call.frame, t->key);
}

void GameRenderer::PhotoPresent(nvrhi::ICommandList* cl, nvrhi::ITexture* frame_image) {
  while (!retired_photo_.empty() && retired_photo_.front().first + kPhotoRetireFrames < frame_) retired_photo_.pop_front();
  if (photo_read_frame_ != frame_) {
    photo_ = PhotoSettingsNow();
    photo_read_frame_ = frame_;
  }
  if (photo_hidden_ != photo_hidden_logged_ && (photo_hidden_logged_ == 0 || frame_ % 600 == 0)) {
    Logf(LogLevel::kInfo, "rexgpu-native: photo mode: %llu draws left out so far",
         static_cast<unsigned long long>(photo_hidden_));
    photo_hidden_logged_ = photo_hidden_;
  }
  // The depth of field's depth: made when it is first wanted, retired when photo mode ends.
  const bool want_depth = photo_.active && photo_.dof > 0.0f;
  if (want_depth && !scene_photo_) {
    scene_photo_ = std::make_unique<ScenePhotoDepth>(device_, options_.ao.rules);
    if (!scene_photo_->Initialize()) {
      Logf(LogLevel::kWarning, "rexgpu-native: photo mode: no depth of field (its depth passes could not be made)");
      scene_photo_.reset();
    }
  } else if (!photo_.active && scene_photo_) {
    retired_photo_.emplace_back(frame_, std::move(scene_photo_));
  }
  if (!PhotoGradeNeeded(photo_) || !frame_image) return;
  if (!photo_grade_ && !photo_grade_failed_) {
    photo_grade_ = std::make_unique<PhotoGrade>(device_);
    if (!photo_grade_->Initialize()) {
      photo_grade_.reset();
      photo_grade_failed_ = true;
      return;
    }
    Logf(LogLevel::kInfo, "rexgpu-native: photo mode: photo grade ready");
  }
  if (!photo_grade_) return;
  const nvrhi::TextureDesc& td = frame_image->getDesc();
  const uint32_t w = photo_frame_w_ > 0 ? uint32_t(photo_frame_w_) : td.width;
  const uint32_t h = photo_frame_h_ > 0 ? uint32_t(photo_frame_h_) : td.height;
  const PhotoGrade::Depth depth = scene_photo_ ? scene_photo_->DepthFor(frame_, w, h) : PhotoGrade::Depth{};
  photo_grade_->Apply(cl, frame_image, w, h, photo_, depth);
}

}  // namespace nr
