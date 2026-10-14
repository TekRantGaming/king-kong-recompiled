#include "backend/game_renderer.h"
#include "backend/log.h"
#include "plugin/native_effect_stats.h"

namespace nr {
namespace {
std::mutex stats_mutex;
NrEffectStats effect_stats = {sizeof(NrEffectStats), 0, -1, -1, -1, -1, -1};
}
bool ReadEffectStats(NrEffectStats* out) {
  if (!out || out->size != sizeof(NrEffectStats)) return false;
  std::lock_guard lock(stats_mutex);
  *out = effect_stats;
  return true;
}

void GameRenderer::UpdateEffects() {
  if (!options_.effect_settings || effects_frame_ == frame_) return;
  effects_frame_ = frame_;
  const EffectSettings settings = options_.effect_settings();
  options_.ao.quality = settings.ao_quality;
  if (scene_ao_) scene_ao_->SetQuality(settings.ao_quality);
  options_.gi = settings.gi;
  options_.lighting = settings.lighting;
  options_.shadow_scale = settings.shadow_scale;
  if (!settings.lighting.shadows_on()) lighting_mask_pitches_.clear();
  // Off never waits for a worker. Ready resources are installed only at a frame boundary.
  if (gi_build_.valid() && gi_build_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    scene_gi_ = gi_build_.get();
    gi_build_failed_ = !scene_gi_;
    if (scene_gi_) {
      if (!gi_timer_) gi_timer_ = std::make_unique<AoGpuTimer>(device_, "gi");
      scene_gi_->SetTimer(gi_timer_.get());
    }
  }
  if (!settings.gi.enabled()) {
    if (scene_gi_) retired_gi_.emplace_back(frame_, std::move(scene_gi_));
    gi_build_failed_ = false;
  } else if (scene_gi_) {
    scene_gi_->SetConfig(settings.gi);
  } else if (!gi_build_.valid() && !gi_build_failed_) {
    gi_build_ = std::async(std::launch::async, [this, device = device_, config = settings.gi, rules = options_.ao.rules] {
      std::shared_lock creation_lock(creation_mutex_);
      auto scene = std::make_unique<SceneGi>(device, config, rules);
      if (!scene->Initialize()) scene.reset();
      return scene;
    });
  }
  if (lighting_build_.valid() && lighting_build_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    scene_lighting_ = lighting_build_.get();
    lighting_build_failed_ = !scene_lighting_;
  }
  if (!settings.lighting.any()) {
    if (scene_lighting_) {
      if (lighting_.color_slot) RetireSlot(kTex2D, lighting_.color_slot);
      if (lighting_.depth_slot) RetireSlot(kTex2D, lighting_.depth_slot);
      lighting_.color_slot = lighting_.depth_slot = 0;
      lighting_.color_slot_texture = lighting_.depth_slot_texture = nullptr;
      retired_lighting_.emplace_back(frame_, std::move(scene_lighting_));
    }
    lighting_build_failed_ = false;
  } else if (scene_lighting_) {
    if (!settings.lighting.water_on()) {
      for (const auto& image : scene_lighting_->ReleaseSsrImages()) if (image) RetireResource(image);
      if (lighting_.color_slot) RetireSlot(kTex2D, lighting_.color_slot);
      if (lighting_.depth_slot) RetireSlot(kTex2D, lighting_.depth_slot);
      lighting_.color_slot = lighting_.depth_slot = 0;
      lighting_.color_slot_texture = lighting_.depth_slot_texture = nullptr;
    }
    scene_lighting_->SetConfig(settings.lighting);
  } else if (!lighting_build_.valid() && !lighting_build_failed_) {
    auto config = settings.lighting;
    // Both pipeline sets are prepared on the worker. Disabled passes do not execute.
    config.water = WaterReflections::kSsr;
    config.volumetrics = Volumetrics::kHigh;
    lighting_build_ = std::async(std::launch::async, [this, device = device_, config] {
      std::shared_lock creation_lock(creation_mutex_);
      auto scene = std::make_unique<SceneLighting>(device, config);
      if (!scene->Initialize()) scene.reset();
      return scene;
    });
  }
  while (!retired_gi_.empty() && retired_gi_.front().first + 8 < frame_) retired_gi_.pop_front();
  while (!retired_lighting_.empty() && retired_lighting_.front().first + 8 < frame_) retired_lighting_.pop_front();
}

void GameRenderer::PublishEffectTimes() {
  if (std::getenv("KK_DEV_EFFECT_TEST") && frame_ % 300 == 0)
    Logf(LogLevel::kInfo, "rexgpu-native: live effect resources frame %llu: GI %u lighting %u retired GI %zu lighting %zu workers %u",
         static_cast<unsigned long long>(frame_), unsigned(bool(scene_gi_)), unsigned(bool(scene_lighting_)),
         retired_gi_.size(), retired_lighting_.size(), unsigned(gi_build_.valid()) + unsigned(lighting_build_.valid()));
  NrEffectStats s = {sizeof(NrEffectStats), uint32_t(gi_build_.valid() || lighting_build_.valid()), -1, -1, -1, -1, -1};
  if (ao_timer_ && ao_settings_.mode) s.ao_ms = ao_timer_->milliseconds();
  if (gi_timer_ && scene_gi_) s.gi_ms = gi_timer_->milliseconds();
  if (scene_lighting_) {
    auto ms = [&](int i) { auto* t = scene_lighting_->timer(i); return t ? t->milliseconds() : -1.0; };
    if (options_.lighting.shadows_on()) s.shadows_ms = ms(0);
    if (options_.lighting.water_on()) s.water_ms = ms(1);
    if (options_.lighting.volumetrics_on()) s.volumetric_ms = ms(2);
  }
  std::lock_guard lock(stats_mutex);
  effect_stats = s;
}
}  // namespace nr
