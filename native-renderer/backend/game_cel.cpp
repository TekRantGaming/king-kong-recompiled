// The game renderer's hooks for the cel shading (scene_cel.h, docs/cel.md): what each draw, clear,
// resolve and Present tells it, and its live setting (made on a worker when turned on, parked when
// turned off, as the GI). The renderer's files call these at a handful of points marked "Cel hook
// (game_cel.cpp)"; with native_cel_shading off nothing of it is made (scene_cel_ stays null) and
// every frame is what it was.

#include <chrono>

#include "backend/game_renderer.h"
#include "backend/log.h"
#include "kknr/render_targets.h"

namespace nr {

namespace {

template <class T>
bool Ready(const std::future<T>& f) {
  return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

constexpr uint64_t kCelParkFrames = 300;

}  // namespace

void GameRenderer::CelInitialize() {
  cel_user_gpu_time_ = options_.cel.gpu_time;
  if (!options_.cel.enabled) return;  // with live settings, made on a worker when turned on (UpdateCel)
  cel_timer_ = std::make_unique<AoGpuTimer>(device_, "cel");
  cel_timer_->set_quiet(!options_.cel.gpu_time);
  scene_cel_ = std::make_unique<SceneCel>(device_, options_.cel, options_.ao.rules);
  if (!scene_cel_->Initialize()) {
    Logf(LogLevel::kWarning, "rexgpu-native: cel shading: unavailable (its shaders could not be made), it stays off");
    scene_cel_.reset();
    cel_timer_.reset();
    return;
  }
  scene_cel_->SetTimer(cel_timer_.get());
}

void GameRenderer::CelDrawHook(nvrhi::ICommandList* cl, const DrawCall& call, const dev::DeviceView& d,
                               const std::array<HostTarget*, 4>& colors, HostTarget* depth, const PipelineRecord& record,
                               const GameShader& vs, const GameShader* ps) {
  const SceneAo::Draw a = AoDescribeDraw(call, d, colors, depth, record, vs, ps);
  // The AO's depth and normals when it worked them out this frame (it runs first, at the same moment).
  const Gtao* ao = scene_ao_ && AoSettingsFor(call.frame).mode == 1 ? scene_ao_->prepared(call.frame) : nullptr;
  scene_cel_->OnDraw(cl, a, ao);
  if (scene_cel_->last_applied() && (Dump(call.frame) || (options_.ao.dump_every && call.frame % options_.ao.dump_every == 0)))
    Logf(LogLevel::kInfo, "rexgpu-native:   cel shading applied before draw %u of frame %llu", call.index_in_frame,
         static_cast<unsigned long long>(call.frame));
}

void GameRenderer::CelClearHook(const ClearCall& call, const HostTarget* depth) {
  if (call.depth && depth) scene_cel_->OnDepthClear(call.frame, depth->key);
}

void GameRenderer::CelResolveHook(const ResolveCall& call) {
  const kknr::ResolveFlags flags{call.flags};
  if (!flags.ClearDepthStencil() || !call.depth_stencil) return;
  dev::DeviceView d(call.device ? memory_.Virtual(call.device) : nullptr);
  if (!d.valid()) return;
  const uint32_t info = d.u32(dev::kRbDepthInfo), pitch = d.u32(dev::kRbSurfaceInfo) & 0x3FFF;
  if (const HostTarget* t = FindTarget(true, info & 0xFFF, pitch, (info >> 16) & 1)) scene_cel_->OnDepthClear(call.frame, t->key);
}

void GameRenderer::CelPresentHook() {
  if (cel_timer_) cel_timer_->FrameEnd();
}

void GameRenderer::UpdateCel(const CelConfig& want) {
  options_.cel = want;
  options_.cel.gpu_time = cel_user_gpu_time_;
  if (Ready(cel_build_)) {
    std::unique_ptr<SceneCel> built = cel_build_.get();
    if (!built) {
      cel_build_failed_ = true;
      Logf(LogLevel::kWarning, "rexgpu-native: live settings: the cel shading could not be made; it stays off");
    } else {
      cel_parked_ = std::move(built);
      cel_parked_frame_ = frame_;
    }
  }
  if (!want.enabled) {
    cel_build_failed_ = false;
    if (scene_cel_) {
      cel_parked_ = std::move(scene_cel_);
      cel_parked_frame_ = frame_;
    }
    if (cel_parked_ && cel_parked_frame_ + kCelParkFrames < frame_) retired_cel_.emplace_back(frame_, std::move(cel_parked_));
    return;
  }
  if (!scene_cel_ && cel_parked_) {
    scene_cel_ = std::move(cel_parked_);
    if (!cel_timer_) {
      cel_timer_ = std::make_unique<AoGpuTimer>(device_, "cel");
      cel_timer_->set_quiet(!cel_user_gpu_time_);
    }
    scene_cel_->SetTimer(cel_timer_.get());
    ++live_changes_;
  }
  if (scene_cel_) {
    scene_cel_->SetConfig(options_.cel);
  } else if (!cel_build_.valid() && !cel_build_failed_) {
    cel_build_ = std::async(std::launch::async, [this, device = device_, config = options_.cel, rules = options_.ao.rules] {
      std::shared_lock creation_lock(creation_mutex_);
      auto scene = std::make_unique<SceneCel>(device, config, rules);
      if (!scene->Initialize()) scene.reset();
      return scene;
    });
  }
}

}  // namespace nr
