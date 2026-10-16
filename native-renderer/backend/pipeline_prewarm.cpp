#include "backend/pipeline_prewarm.h"

#include <chrono>
#include <thread>

#include "backend/driver_pipeline_cache.h"
#include "backend/game_renderer.h"
#include "backend/guest_memory.h"
#include "backend/shader_library.h"

namespace nr {

// No game yet: no guest memory either (the shaders come from the pack by their hashes).
class PipelinePrewarm::NoGuestMemory final : public GuestMemory {
 public:
  const uint8_t* Virtual(uint32_t) const override { return nullptr; }
  const uint8_t* Physical(uint32_t) const override { return nullptr; }
};

PipelinePrewarm::PipelinePrewarm(nvrhi::IDevice* device) : device_(device) {}

PipelinePrewarm::~PipelinePrewarm() { Stop(); }

bool PipelinePrewarm::Start(const Settings& settings) {
  if (renderer_) return false;
  memory_ = std::make_unique<NoGuestMemory>();
  shaders_ = std::make_unique<ShaderLibrary>(device_, *memory_);
  // As Backend::Initialize sets them up for the game.
  shaders_->SetRenderScaleAware(settings.live_layout || settings.shaders_aware ||
      (settings.upscale.enabled() && settings.upscale.quality != UpscaleQuality::kNative));
  shaders_->SetWaterReflections(!settings.live_layout && settings.lighting.water_on());
  shaders_->Initialize(settings.shader_pack, settings.dxc);
  auto renderer = std::make_unique<GameRenderer>(device_, *memory_, kknr::GuestMemory{}, shaders_.get());
  GameRenderer::Options& o = renderer->options();
  o.table_capacities = settings.table_capacities;
  o.render_scale_x = settings.render_scale_x;
  o.render_scale_y = settings.render_scale_y;
  o.shadow_scale = settings.shadow_scale;
  o.flip_front_face = settings.flip_front_face;
  o.tessellation_supported = settings.tessellation_supported;
  o.lighting = settings.lighting;
  o.lighting.census_every = 0;
  o.lighting.gpu_time = false;
  o.upscale = settings.upscale;
  o.live_layout = settings.live_layout;
  o.ao = settings.ao;  // before Initialize: the ambient-only AO adds a binding
  // Only the motion layout/shaders and input scale are needed, not an SDK context.
  // All temporal upscalers use the same object-motion pipeline layout.
  if (o.upscale.enabled()) o.upscale.kind = UpscalerKind::kBilinear;
  o.async_pipelines = true;  // the prewarm is the workers
  if (!renderer->Initialize()) {
    renderer.reset();
    shaders_.reset();
    return false;
  }
  shaders_->SetAmbientOcclusion(renderer->ao_ambient_table());
  if (!settings.live_layout) shaders_->SetReplacements(renderer->LightingReplacements());
  renderer_ = std::move(renderer);
  // The driver's cache first: nothing may create a pipeline while its file is read.
  if (settings.driver_cache) renderer_->SetDriverPipelineCache(settings.driver_cache, settings.driver_cache_path);
  for (const std::string& list : settings.lists) renderer_->LoadPipelineList(list);
  listed_ = renderer_->stats().list_records;
  return true;
}

PipelinePrewarm::Progress PipelinePrewarm::progress() {
  Progress p;
  if (!renderer_) return p;
  const GameRenderer::WorkerProgress w = renderer_->worker_progress();
  p.listed = listed_;
  p.queued = w.queued;
  p.made = w.made;
  p.failed = w.failed;
  // Written by Start on its own thread before any caller can see renderer_ running.
  p.waiting = renderer_->stats().prewarm_deferred;
  p.other_kind = renderer_->stats().prewarm_other_kind;
  p.done = w.waiting == 0 && w.active == 0;
  return p;
}

bool PipelinePrewarm::Wait(double seconds) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
  while (renderer_ && !progress().done) {
    if (std::chrono::steady_clock::now() >= until) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

std::vector<PipelineRecord> PipelinePrewarm::waiting_records() {
  return renderer_ ? renderer_->deferred_records() : std::vector<PipelineRecord>{};
}

void PipelinePrewarm::Stop() {
  // The renderer stops its workers after their current pipeline and saves the driver's cache.
  renderer_.reset();
  shaders_.reset();
}

}  // namespace nr
