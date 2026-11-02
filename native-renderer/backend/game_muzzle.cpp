// The game renderer's hooks for the muzzle light (muzzle_light.h, docs/lighting.md). The renderer's files call these at
// a handful of points marked "Muzzle hook (game_muzzle.cpp)". With native_muzzle_light off nothing of it is made
// (scene_muzzle_ stays null); with it on, a frame with no flash costs one look at the list of flashes.

#include "backend/game_renderer.h"
#include "backend/log.h"
#include "kknr/render_targets.h"

namespace nr {

void GameRenderer::MuzzleInitialize() {
  muzzle_user_gpu_time_ = options_.muzzle.gpu_time;
  if (options_.muzzle.enabled) UpdateMuzzle(options_.muzzle);
}

void GameRenderer::UpdateMuzzle(const MuzzleConfig& want) {
  options_.muzzle = want;
  options_.muzzle.gpu_time = muzzle_user_gpu_time_;
  if (!want.enabled || scene_muzzle_ || muzzle_failed_) return;
  muzzle_timer_ = std::make_unique<AoGpuTimer>(device_, "muzzle");
  muzzle_timer_->set_quiet(!muzzle_user_gpu_time_);
  scene_muzzle_ = std::make_unique<SceneMuzzle>(device_, options_.ao.rules);
  if (!scene_muzzle_->Initialize()) {
    Logf(LogLevel::kWarning, "rexgpu-native: muzzle light: unavailable (its shaders could not be made), it stays off");
    scene_muzzle_.reset();
    muzzle_timer_.reset();
    muzzle_failed_ = true;
    return;
  }
  scene_muzzle_->SetTimer(muzzle_timer_.get());
}

void GameRenderer::MuzzleDrawHook(nvrhi::ICommandList* cl, const DrawCall& call, const dev::DeviceView& d,
                                  const std::array<HostTarget*, 4>& colors, HostTarget* depth, const PipelineRecord& record,
                                  const GameShader& vs, const GameShader* ps) {
  if (!options_.muzzle.enabled || !scene_muzzle_->Wanted(call.frame)) return;
  const SceneAo::Draw a = AoDescribeDraw(call, d, colors, depth, record, vs, ps);
  const Gtao* ao = scene_ao_ && AoSettingsFor(call.frame).mode == 1 ? scene_ao_->prepared(call.frame) : nullptr;
  scene_muzzle_->OnDraw(cl, a, ao);
}

void GameRenderer::MuzzleClearHook(const ClearCall& call, const HostTarget* depth) {
  if (call.depth && depth && options_.muzzle.enabled && scene_muzzle_->Wanted(call.frame))
    scene_muzzle_->OnDepthClear(call.frame, depth->key);
}

void GameRenderer::MuzzleResolveHook(const ResolveCall& call) {
  const kknr::ResolveFlags flags{call.flags};
  if (!flags.ClearDepthStencil() || !call.depth_stencil || !options_.muzzle.enabled || !scene_muzzle_->Wanted(call.frame)) return;
  dev::DeviceView d(call.device ? memory_.Virtual(call.device) : nullptr);
  if (!d.valid()) return;
  const uint32_t info = d.u32(dev::kRbDepthInfo), pitch = d.u32(dev::kRbSurfaceInfo) & 0x3FFF;
  if (const HostTarget* t = FindTarget(true, info & 0xFFF, pitch, (info >> 16) & 1)) scene_muzzle_->OnDepthClear(call.frame, t->key);
}

void GameRenderer::MuzzlePresentHook() {
  if (muzzle_timer_) muzzle_timer_->FrameEnd();
}

}  // namespace nr
