// Settings applied while the game runs (effect_settings.h): the graphics panel (F8) and the in-game
// settings pages change the ambient occlusion's resolution, the global illumination, the lighting
// options, the temporal upscaler, FXAA and the texture filtering between two frames.
//
// Once a frame, at its first clear, draw or resolve (before any target of the frame is taken):
//  - what costs nothing to change is set at once (strengths, the AO resolution, FXAA, sharpness);
//  - an effect that needs resources of its own (GI, the lighting options' passes) is made on a worker
//    thread while the game keeps drawing without it, and put in at the frame boundary after it is
//    ready. Turned off, it is kept aside for kParkFrames (an A/B switch back is instant), then retired;
//  - a new upscaler (kind, mode, DLSS preset) is made on a worker too (its DLL, NGX), and swapped in at a
//    frame boundary; the old one is retired. Its passes are made once and kept (UpscalePasses);
//  - retired objects are destroyed kRetireFrames later, when no frame in flight uses them. The SDKs'
//    own waits for the GPU take the host's queue lock (upscaler.h, SetUpscalerIdleWait).
// No real draw is skipped or waited for: until a variant shader or pipeline is ready, a draw uses the
// game's own (LiveLightingShader).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "backend/game_renderer.h"
#include "backend/game_upscale.h"
#include "backend/log.h"
#include "backend/upscaler.h"
#include "plugin/native_effect_stats.h"

namespace nr {
namespace {

std::mutex stats_mutex;
NrEffectStats effect_stats = [] {
  NrEffectStats s{};
  s.size = sizeof(NrEffectStats);
  s.ao_ms = s.gi_ms = s.shadows_ms = s.water_ms = s.volumetric_ms = s.upscaler_ms = -1;
  s.tessellation_ms=-1;
  std::snprintf(s.upscaler, sizeof(s.upscaler), "off");
  return s;
}();

constexpr uint64_t kRetireFrames = 8;
constexpr uint64_t kParkFrames = 300;

template <class T>
bool Ready(const std::future<T>& f) {
  return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

uint32_t RoundSize(uint32_t guest, float scale) {
  return uint32_t(std::max(1L, std::lround(double(guest) * double(scale))));
}

double MsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// A change that makes the upscaler again (its kind, mode or DLSS preset); the rest applies per frame.
bool SameUpscaler(const UpscaleConfig& a, const UpscaleConfig& b) {
  if (a.kind != b.kind) return false;
  if (!a.enabled() || a.kind == UpscalerKind::kBilinear) return a.quality == b.quality || !a.enabled();
  return a.quality == b.quality && a.dlss_preset == b.dlss_preset;
}

}  // namespace

bool ReadEffectStats(NrEffectStats* out) {
  if (!out || out->size != sizeof(NrEffectStats)) return false;
  std::lock_guard lock(stats_mutex);
  *out = effect_stats;
  return true;
}

void GameRenderer::UpdateEffects() {
  if (!options_.effect_settings || effects_frame_ == frame_) return;
  effects_frame_ = frame_;
  const auto start = std::chrono::steady_clock::now();
  const EffectSettings s = options_.effect_settings();
  tessellation_=s.tessellation && options_.tessellation_supported;
  tessellation_smoothness_=std::isfinite(s.tessellation_smoothness)?std::clamp(s.tessellation_smoothness,0.0f,1.0f):0.75f;
  tessellation_detail_=std::clamp(s.tessellation_detail,1u,16u);
  if ((tessellation_ || TessellationBenchRequested()) && !tessellation_timer_) {
    tessellation_timer_=std::make_unique<AoGpuTimer>(device_,"Ann tessellation");
    tessellation_timer_->EnableTrace(".tess.csv");
  }
  // Ambient occlusion: the resolution (GTAO makes its textures for it at its next pass).
  options_.ao.quality = s.ao_quality;
  if (scene_ao_) scene_ao_->SetQuality(s.ao_quality);
  UpdateUpscaler(s.upscale);
  // FXAA: the upscalers anti-alias themselves (UpscaleSetup turns it off with one).
  fxaa_wanted_ = s.fxaa;
  options_.fxaa = upscale_ ? 0u : s.fxaa;
  if (s.anisotropic_override != options_.anisotropic_override) {
    options_.anisotropic_override = s.anisotropic_override;
    FlushSamplers();
  }
  UpdateGi(s.gi);
  UpdateLighting(s.lighting, s.shadow_scale);
  while (!retired_gi_.empty() && retired_gi_.front().first + kRetireFrames < frame_) retired_gi_.pop_front();
  while (!retired_lighting_.empty() && retired_lighting_.front().first + kRetireFrames < frame_) retired_lighting_.pop_front();
  while (!retired_upscale_.empty() && retired_upscale_.front().first + kRetireFrames < frame_) {
    const auto destroy = std::chrono::steady_clock::now();
    retired_upscale_.pop_front();
    live_last_change_ms_ = std::max(live_last_change_ms_, MsSince(destroy));
  }
  while (!retired_upscalers_.empty() && retired_upscalers_.front().first + kRetireFrames < frame_) retired_upscalers_.pop_front();
  const double ms = MsSince(start);
  if (ms > 30.0)
    Logf(LogLevel::kWarning, "rexgpu-native: live settings: frame %llu spent %.1f ms applying a change",
         static_cast<unsigned long long>(frame_), ms);
}

// ---------------------------------------------------------------- the upscaler

void GameRenderer::UpdateUpscaler(UpscaleConfig want) {
  // A live settings callback must obey the same replay lifetime restriction
  // as startup and pipeline prewarm. Keep the player's saved choice intact.
  if (options_.table_capacities != kNativeTableCapacities) want.kind = UpscalerKind::kOff;
  // What only the start sets (the point, debugging, timers, rules) stays as it was.
  want.point = upscale_initial_.point;
  want.debug = upscale_initial_.debug;
  want.gpu_time = upscale_initial_.gpu_time;
  want.rules_file = upscale_initial_.rules_file;
  if (Ready(upscale_build_)) {
    PreparedUpscaler prepared = upscale_build_.get();
    if (SameUpscaler(prepared.config, want)) {
      InstallUpscaler(want, &prepared);
    } else if (prepared.upscaler) {
      // Asked for something else since: never used. Destroyed with the others (its SDK waits for the GPU).
      retired_upscalers_.emplace_back(frame_, std::move(prepared.upscaler));
    }
  }
  if (Ready(dlss_warm_)) {
    // NGX stays started for the renderer's life: starting it takes seconds, and a switch back to DLSS
    // (an A/B comparison) would wait that long each time.
    dlss_keepalive_ = dlss_warm_.get();
  }
  if (!SameUpscaler(want, upscale_requested_)) {
    if (!want.enabled() || want.kind == UpscalerKind::kBilinear) {
      // Nothing to make: at once, even while another one is being made (it is dropped when ready).
      InstallUpscaler(want, nullptr);
    } else if (!upscale_build_.valid()) {
      // The SDK (its DLL, NGX's start) on a worker; the game draws with what it has meanwhile.
      const uint32_t gw = options_.guest_frame_width, gh = options_.guest_frame_height;
      const float sx = options_.render_scale_x, sy = options_.render_scale_y;
      upscale_build_ = std::async(std::launch::async, [device = device_, want, gw, gh, sx, sy] {
        const auto start = std::chrono::steady_clock::now();
        PreparedUpscaler p;
        p.config = want;
        p.upscaler = CreateUpscaler(device, want, &p.chosen, &p.why);
        if (p.upscaler) {
          // The SDK's context too, for the sizes UpscaleSetup will give it (the frame's).
          const UpscalerSize out{RoundSize(gw, sx), RoundSize(gh, sy)};
          UpscalerSize in = p.upscaler->InputSize(out);
          in.width = std::clamp(in.width, 1u, out.width);
          in.height = std::clamp(in.height, 1u, out.height);
          p.upscaler->Prepare(in, out);
        }
        p.ms = MsSince(start);
        return p;
      });
    }
  }
  if (!upscale_) {
    upscale_requested_.sharpness = want.sharpness;
    upscale_requested_.reactive = want.reactive;
    upscale_requested_.mip_bias = want.mip_bias;
    upscale_requested_.mip_bias_auto = want.mip_bias_auto;
    return;
  }
  // Applied to the next upscale: sharpness, the reactive mask, the mip bias of the scene's draws.
  GameUpscale& u = *upscale_;
  u.config.sharpness = want.sharpness;
  u.config.reactive = want.reactive;
  u.config.mip_bias = want.mip_bias;
  u.config.mip_bias_auto = want.mip_bias_auto;
  const float bias = UpscaleMipBiasFor(u);
  if (bias != u.mip_bias) {
    u.mip_bias = bias;
    FlushSamplers();  // the samplers carry the bias (BindSampler)
  }
}

void GameRenderer::InstallUpscaler(const UpscaleConfig& want, PreparedUpscaler* prepared) {
  const auto start = std::chrono::steady_clock::now();
  const std::string before = upscale_ ? upscale_->name : std::string("off");
  if (upscale_) retired_upscale_.emplace_back(frame_, std::move(upscale_));
  options_.upscale = want;
  upscale_requested_ = want;
  if (want.enabled() && UpscaleSetup(prepared) && !UpscaleInitialize()) {
    Logf(LogLevel::kWarning, "rexgpu-native: live settings: the upscaler's passes could not be made; no upscaler");
    upscale_.reset();
  }
  shaders_->SetMotionPrewarm(bool(upscale_));
  // Without an upscaler the frame-size targets are one generation again: the output twins go.
  if (!upscale_) PurgeOutputGeneration();
  FlushSamplers();
  options_.fxaa = upscale_ ? 0u : fxaa_wanted_;
  ++live_changes_;
  live_last_change_ms_ = MsSince(start);
  Logf(LogLevel::kInfo, "rexgpu-native: live settings: upscaler %s -> %s (%.1f ms at the frame boundary%s%s)",
       before.c_str(), upscale_ ? upscale_->name.c_str() : "off", live_last_change_ms_,
       prepared ? ", made on a worker in " : "", prepared ? (std::to_string(int(prepared->ms)) + " ms").c_str() : "");
}

void GameRenderer::PurgeOutputGeneration() {
  for (auto it = targets_.begin(); it != targets_.end();) {
    if (!(it->first & kOutputGeneration) || !it->second) {
      ++it;
      continue;
    }
    HostTarget* t = it->second.get();
    stats_.edram_target_bytes -= std::min(stats_.edram_target_bytes, t->allocation_bytes);
    if (stats_.edram_targets) --stats_.edram_targets;
    blit_sets_.erase(t->texture.Get());
    RetireResource(t->texture);
    it = targets_.erase(it);
  }
  framebuffers_.clear();
  framebuffer_used_.clear();
}

void GameRenderer::FlushSamplers() {
  // Each distinct sampler takes a slot of the sampler table for good (BindSampler): a change of the
  // anisotropy or of the mip bias would fill it. The draws of this frame take new ones.
  for (auto& [key, entry] : samplers_) {
    if (entry.second) RetireSlot(kSamplers, entry.second);
    if (entry.first) RetireResource(entry.first);
  }
  samplers_.clear();
}

// ---------------------------------------------------------------- global illumination

void GameRenderer::UpdateGi(const GiConfig& want) {
  options_.gi = want;
  if (Ready(gi_build_)) {
    std::unique_ptr<SceneGi> built = gi_build_.get();
    if (!built) {
      gi_build_failed_ = true;
      Logf(LogLevel::kWarning, "rexgpu-native: live settings: GI could not be made; it stays off");
    } else {
      gi_parked_ = std::move(built);
      gi_parked_frame_ = frame_;
    }
  }
  if (!want.enabled()) {
    gi_build_failed_ = false;
    if (scene_gi_) {
      gi_parked_ = std::move(scene_gi_);
      gi_parked_frame_ = frame_;
    }
    if (gi_parked_ && gi_parked_frame_ + kParkFrames < frame_) retired_gi_.emplace_back(frame_, std::move(gi_parked_));
    return;
  }
  if (!scene_gi_ && gi_parked_) {
    scene_gi_ = std::move(gi_parked_);
    if (!gi_timer_) {
      gi_timer_ = std::make_unique<AoGpuTimer>(device_, "gi");
      gi_timer_->set_quiet(!gi_user_gpu_time_);
    }
    scene_gi_->SetTimer(gi_timer_.get());
    ++live_changes_;
  }
  if (scene_gi_) {
    scene_gi_->SetConfig(want);
  } else if (!gi_build_.valid() && !gi_build_failed_) {
    gi_build_ = std::async(std::launch::async, [this, device = device_, config = want, rules = options_.ao.rules] {
      std::shared_lock creation_lock(creation_mutex_);
      auto scene = std::make_unique<SceneGi>(device, config, rules);
      if (!scene->Initialize()) scene.reset();
      return scene;
    });
  }
}

// ---------------------------------------------------------------- the lighting options

void GameRenderer::UpdateLighting(const LightingConfig& want, float shadow_scale) {
  options_.lighting = want;
  options_.lighting.census_every = lighting_initial_.census_every;
  options_.lighting.gpu_time = lighting_initial_.gpu_time;
  // Live settings are sampled each frame; keep the session's budget reduction.
  options_.shadow_scale = shadow_scale > 1.0f && memory_shadow_limit_ > 0.0f ?
      std::min(shadow_scale, memory_shadow_limit_) : shadow_scale;
  // The soft shadows' screen-space mask is finer than the frame; the game's own is not.
  if (!want.shadows_on()) lighting_mask_pitches_.clear();
  if (Ready(lighting_build_)) {
    std::unique_ptr<SceneLighting> built = lighting_build_.get();
    if (!built) {
      lighting_build_failed_ = true;
      Logf(LogLevel::kWarning, "rexgpu-native: live settings: the lighting options' passes could not be made");
    } else {
      for (int i = 0; i < 3; ++i)
        if (AoGpuTimer* t = built->timer(i)) t->set_quiet(!lighting_initial_.gpu_time);
      lighting_parked_ = std::move(built);
      lighting_parked_frame_ = frame_;
    }
  }
  auto drop_slots = [this] {
    if (lighting_.color_slot) RetireSlot(kTex2D, lighting_.color_slot);
    if (lighting_.depth_slot) RetireSlot(kTex2D, lighting_.depth_slot);
    lighting_.color_slot = lighting_.depth_slot = 0;
    lighting_.color_slot_texture = lighting_.depth_slot_texture = nullptr;
  };
  if (!want.any()) {
    lighting_build_failed_ = false;
    if (scene_lighting_) {
      drop_slots();
      lighting_parked_ = std::move(scene_lighting_);
      lighting_parked_frame_ = frame_;
    }
    if (lighting_parked_ && lighting_parked_frame_ + kParkFrames < frame_) {
      retired_lighting_.emplace_back(frame_, std::move(lighting_parked_));
    }
    return;
  }
  if (!scene_lighting_ && lighting_parked_) {
    scene_lighting_ = std::move(lighting_parked_);
    ++live_changes_;
  }
  if (scene_lighting_) {
    if (!want.water_on() && scene_lighting_->config().water_on()) {
      // The reflections' copies of the scene go (they are as big as the frame).
      drop_slots();
      for (const auto& image : scene_lighting_->ReleaseSsrImages())
        if (image) RetireResource(image);
    }
    LightingConfig config = want;
    config.gpu_time = true;  // the panel's readout
    scene_lighting_->SetConfig(config);
  } else if (!lighting_build_.valid() && !lighting_build_failed_) {
    LightingConfig config = want;
    // Every pass's pipelines are made on the worker; what is off does not run (SetConfig).
    config.water = WaterReflections::kSsr;
    config.volumetrics = Volumetrics::kHigh;
    config.gpu_time = true;
    lighting_build_ = std::async(std::launch::async, [this, device = device_, config] {
      std::shared_lock creation_lock(creation_mutex_);
      auto scene = std::make_unique<SceneLighting>(device, config);
      if (!scene->Initialize()) scene.reset();
      return scene;
    });
  }
}

// ---------------------------------------------------------------- the panel's readout

GameRenderer::EffectGpuWindow GameRenderer::TakeEffectGpuWindow() {
  EffectGpuWindow w;
  if (ao_timer_) w.ao = ao_timer_->TakeWindow();
  if (gi_timer_) w.gi = gi_timer_->TakeWindow();
  if (upscale_ && upscale_->timer) w.upscaler = upscale_->timer->TakeWindow();
  if (scene_lighting_) {
    AoGpuTimer::Window* out[3] = {&w.shadows, &w.water, &w.volumetrics};
    for (int i = 0; i < 3; ++i)
      if (AoGpuTimer* t = scene_lighting_->timer(i)) *out[i] = t->TakeWindow();
  }
  return w;
}

void GameRenderer::PublishEffectTimes() {
  if (!options_.effect_settings) return;
  NrEffectStats s{};
  s.size = sizeof(NrEffectStats);
  s.ao_ms = s.gi_ms = s.shadows_ms = s.water_ms = s.volumetric_ms = s.upscaler_ms = -1;
  s.tessellation_ms=tessellation_timer_ && (tessellation_draws_this_frame_ || TessellationBenchRequested())
                        ? tessellation_timer_->milliseconds() : -1;
  s.tessellation_draws=tessellation_draws_this_frame_;
  s.preparing = (gi_build_.valid() ? 1u : 0u) | (lighting_build_.valid() ? 2u : 0u) | (upscale_build_.valid() ? 4u : 0u) |
                (shaders_->lighting_variants_pending() ? 8u : 0u);
  if (ao_timer_ && AoSettingsFor(ao_settings_frame_).mode) s.ao_ms = ao_timer_->milliseconds();
  if (gi_timer_ && scene_gi_) s.gi_ms = gi_timer_->milliseconds();
  if (scene_lighting_) {
    auto ms = [&](int i) {
      const AoGpuTimer* t = scene_lighting_->timer(i);
      return t ? t->milliseconds() : -1.0;
    };
    if (options_.lighting.shadows_on()) s.shadows_ms = ms(0);
    if (options_.lighting.water_on()) s.water_ms = ms(1);
    if (options_.lighting.volumetrics_on()) s.volumetric_ms = ms(2);
  }
  const uint32_t out_w = RoundSize(options_.guest_frame_width, options_.render_scale_x);
  const uint32_t out_h = RoundSize(options_.guest_frame_height, options_.render_scale_y);
  s.output_width = out_w;
  s.output_height = out_h;
  if (upscale_) {
    if (upscale_->timer) s.upscaler_ms = upscale_->timer->milliseconds();
    s.upscaler_kind = uint32_t(upscale_->kind);
    s.input_width = RoundSize(options_.guest_frame_width, upscale_->in_x);
    s.input_height = RoundSize(options_.guest_frame_height, upscale_->in_y);
    s.mip_bias = upscale_->mip_bias;
    std::snprintf(s.upscaler, sizeof(s.upscaler), "%s", upscale_->name.c_str());
  } else {
    s.input_width = out_w;
    s.input_height = out_h;
    std::snprintf(s.upscaler, sizeof(s.upscaler), "off");
  }
  const bool ao = AoSettingsFor(ao_settings_frame_).mode > 0 && (scene_ao_ || ao_);
  s.applied = (ao ? 1u : 0u) | (scene_gi_ ? 2u : 0u) | (options_.lighting.shadows_on() && live_draws_.shadows ? 4u : 0u) |
              (options_.lighting.water_on() && scene_lighting_ && live_draws_.water ? 8u : 0u) |
              (options_.lighting.volumetrics_on() && scene_lighting_ ? 16u : 0u) | (options_.fxaa ? 32u : 0u) |
              (upscale_ ? 64u : 0u);
  s.anisotropy = options_.anisotropic_override;
  s.gi_level = scene_gi_ ? uint32_t(options_.gi.level) : 0u;
  s.shadow_scale = LightingShadowScale();
  s.shadow_draws = live_draws_.shadows;
  s.water_draws = live_draws_.water;
  s.shadow_draws_waiting = live_draws_.shadows_waiting;
  s.water_draws_waiting = live_draws_.water_waiting;
  live_draws_ = {};
  s.api = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN ? 1u : 2u;
  s.vendor = vendor_;
  s.frame = frame_;
  s.live_changes = live_changes_;
  s.last_change_ms = live_last_change_ms_;
  s.sampler_slots = uint32_t(samplers_.size());
  s.targets = uint32_t(targets_.size());
  s.retired = uint32_t(retired_resources_.size() + retired_gi_.size() + retired_lighting_.size() +
                       retired_upscale_.size() + retired_upscalers_.size());
  if (std::getenv("KK_DEV_EFFECT_TEST") && frame_ % 300 == 0)
    Logf(LogLevel::kInfo,
         "rexgpu-native: live settings frame %llu: GI %u (parked %u), lighting %u (parked %u), upscaler %s, %u targets, "
         "%u samplers, %u retired, %llu changes, workers %u",
         static_cast<unsigned long long>(frame_), unsigned(bool(scene_gi_)), unsigned(bool(gi_parked_)),
         unsigned(bool(scene_lighting_)), unsigned(bool(lighting_parked_)), s.upscaler, s.targets, s.sampler_slots,
         s.retired, static_cast<unsigned long long>(live_changes_), s.preparing);
  std::lock_guard lock(stats_mutex);
  effect_stats = s;
}

}  // namespace nr
