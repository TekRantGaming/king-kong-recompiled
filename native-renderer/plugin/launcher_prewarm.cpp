#include "plugin/launcher_prewarm.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#if NR_HAS_VULKAN
#include <rex/ui/vulkan/provider.h>
#endif

#include "backend/backend.h"
#include "backend/host_device.h"
#include "backend/render_scale.h"
#include "plugin/plugin_settings.h"
#if NR_GAME_RENDERER
#include "plugin/shader_pack_service.h"
#endif
#if NR_GAME_RENDERER
#include "backend/pipeline_prewarm.h"
#endif

REXCVAR_DECLARE(bool, native_launcher_prewarm);
REXCVAR_DECLARE(bool, native_pipeline_cache);
REXCVAR_DECLARE(bool, native_pipeline_list);
REXCVAR_DECLARE(bool, native_vk_pipeline_cache);
REXCVAR_DECLARE(std::string, native_backend);
REXCVAR_DECLARE(bool, native_game);
REXCVAR_DECLARE(bool, native_async_pipelines);

namespace nr {

#if NR_HAS_VULKAN && NR_GAME_RENDERER

namespace {

// What the prewarm makes, read from the cvars on the caller's thread (the launcher's UI thread, which
// also writes them): the thread never touches a cvar.
struct Plan {
  PipelinePrewarm::Settings settings;  // without the driver's cache (the thread's device has it)
  std::string driver_cache_path;       // empty: --native_vk_pipeline_cache is off
};

Plan MakePlan(const PipelinePlace& place, const std::filesystem::path& cache_root) {
  Plan plan;
  const GameSettings game = ReadGameSettings(true);
  rex::system::X_VIDEO_MODE mode;
  rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
  const RenderScaleSetup scale =
      ResolveRenderScale(game.render_scale, game.shadow_scale, game.debug, std::max<uint32_t>(1, mode.display_width),
                         std::max<uint32_t>(1, mode.display_height));
  PipelinePrewarm::Settings& s = plan.settings;
  s.shader_pack = game.shader_pack;
#if NR_GAME_RENDERER
  // The pack the game will open: the user's cache (the launcher builds it there), else beside the game.
  if (s.shader_pack.empty()) s.shader_pack = FindShaderPackForRun(cache_root, true, nullptr);
#else
  (void)cache_root;
#endif
  s.dxc = game.dxc;
  s.render_scale_x = scale.scale.x;
  s.render_scale_y = scale.scale.y;
  s.shadow_scale = scale.shadow_scale;
  s.shaders_aware = scale.shaders_aware;
  s.flip_front_face = game.flip_front_face;
  s.lighting = game.lighting;
  s.upscale = game.upscale;
  s.ao = game.ao;
  s.live_layout = true;  // the game runs with live settings (pipeline_prewarm.h)
  if (REXCVAR_GET(native_pipeline_cache) && !place.empty()) s.lists.push_back(place.records());
  if (REXCVAR_GET(native_pipeline_list)) s.lists.push_back(ShippedPipelineList());
  if (REXCVAR_GET(native_vk_pipeline_cache) && !place.empty()) plan.driver_cache_path = place.driver_cache();
  return plan;
}

class LauncherPrewarm {
 public:
  explicit LauncherPrewarm(Plan plan) : plan_(std::move(plan)), start_(std::chrono::steady_clock::now()) {
    state_ = NR_PREWARM_RUNNING;
    thread_ = std::thread([this] { Run(); });
  }

  void Stop() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    std::lock_guard join_lock(join_mutex_);  // two callers: one joins, the other waits for it
    if (thread_.joinable()) thread_.join();
  }

  void Status(NrPrewarmStatus& s) {
    std::lock_guard lock(mutex_);
    s.state = state_;
    s.listed = uint32_t(progress_[0]);
    s.queued = uint32_t(progress_[1]);
    s.made = uint32_t(progress_[2]);
    s.waiting = uint32_t(progress_[3]);
    s.seconds = std::chrono::duration<double>((finished_ ? end_ : std::chrono::steady_clock::now()) - start_).count();
  }

 private:
  bool Stopping() {
    std::lock_guard lock(mutex_);
    return stop_;
  }

  void Finish(uint32_t state) {
    std::lock_guard lock(mutex_);
    state_ = state;
    finished_ = true;
    end_ = std::chrono::steady_clock::now();
  }

  void Run() {
    // The SDK's provider as the game's start makes it (the same GPU and features), without
    // presentation.
    nr::PrepareVulkanDeviceRequests();  // the graphics pipeline library (task 183)
    std::unique_ptr<rex::ui::vulkan::VulkanProvider> provider = rex::ui::vulkan::VulkanProvider::Create(true, false);
    std::unique_ptr<HostDevice> host = provider ? CreateVulkanHostDevice(*provider) : nullptr;
    if (!host) {
      REXLOG_WARN("rexgpu-native: launcher prewarm: no Vulkan device");
      Finish(NR_PREWARM_FAILED);
      return;
    }
    PipelinePrewarm::Settings settings = plan_.settings;
    settings.tessellation_supported = host->tessellation_supported();
    settings.table_capacities = host->table_capacities();
    if (!plan_.driver_cache_path.empty() && host->pipeline_cache()) {
      settings.driver_cache = host->pipeline_cache();
      settings.driver_cache_path = plan_.driver_cache_path;
    }
    uint32_t state = NR_PREWARM_DONE;
    if (Stopping()) {
      state = NR_PREWARM_STOPPED;  // Play came before the device was up: nothing to do
    } else {
      PipelinePrewarm prewarm(host->device());
      if (!prewarm.Start(settings)) {
        REXLOG_WARN("rexgpu-native: launcher prewarm: the game renderer could not be made");
        state = NR_PREWARM_FAILED;
      } else {
        REXLOG_INFO("rexgpu-native: launcher prewarm started: {} descriptions, {} to make", prewarm.progress().listed,
                    prewarm.progress().queued);
        for (;;) {
          const PipelinePrewarm::Progress p = prewarm.progress();
          std::unique_lock lock(mutex_);
          progress_[0] = p.listed;
          progress_[1] = p.queued;
          progress_[2] = p.made;
          progress_[3] = p.waiting;
          if (p.done) break;
          if (cv_.wait_for(lock, std::chrono::milliseconds(50), [&] { return stop_; })) {
            state = NR_PREWARM_STOPPED;
            break;
          }
        }
        const PipelinePrewarm::Progress p = prewarm.progress();
        prewarm.Stop();  // the driver's pipeline cache is saved here
        REXLOG_INFO("rexgpu-native: launcher prewarm {}: {} of {} pipelines made, {} left to the game, {:.1f} s",
                    state == NR_PREWARM_DONE ? "done" : "stopped", p.made, p.queued, p.waiting + (p.queued - p.made),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count());
      }
    }
    host->WaitForIdle();
    host.reset();
    provider.reset();
    Finish(state);
  }

  Plan plan_;
  std::chrono::steady_clock::time_point start_, end_;
  std::mutex mutex_, join_mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  bool finished_ = false;
  uint32_t state_ = NR_PREWARM_OFF;
  uint64_t progress_[4] = {};
  std::thread thread_;
};

std::mutex g_mutex;
// Never destroyed: a static destructor joining the thread at the process's exit could wait on a
// driver being unloaded. The launcher stops it when it quits without playing (nr_prewarm_stop),
// the game's start does when it plays.
LauncherPrewarm* g_prewarm = nullptr;
bool g_started = false;  // once a process

}  // namespace

uint32_t StartLauncherPrewarm(const std::filesystem::path& cache_root, uint32_t title_id) {
  std::lock_guard lock(g_mutex);
  if (g_started) {
    NrPrewarmStatus s = {};
    if (g_prewarm) g_prewarm->Status(s);
    return s.state;
  }
  if (!REXCVAR_GET(native_launcher_prewarm)) return NR_PREWARM_OFF;
  if (rex::cvar::GetFlagByName("gpu_plugin") != "native") {
    REXLOG_INFO("rexgpu-native: launcher prewarm off: the GPU plugin is not native");
    return NR_PREWARM_OFF;
  }
  if (REXCVAR_GET(native_backend) == "d3d12") {
    REXLOG_INFO("rexgpu-native: launcher prewarm off: it needs Vulkan (native_backend is d3d12)");
    return NR_PREWARM_OFF;
  }
  if (!REXCVAR_GET(native_game) || !REXCVAR_GET(native_async_pipelines)) {
    REXLOG_INFO("rexgpu-native: launcher prewarm off: it needs native_game and native_async_pipelines");
    return NR_PREWARM_OFF;
  }
  g_started = true;
  InstallRuntimeLog();
  g_prewarm = new LauncherPrewarm(MakePlan(PipelinePlaceUnder(cache_root, title_id), cache_root));
  return NR_PREWARM_RUNNING;
}

void StopLauncherPrewarm() {
  LauncherPrewarm* prewarm = nullptr;
  {
    std::lock_guard lock(g_mutex);
    prewarm = g_prewarm;
  }
  // Outside the lock (the status stays readable meanwhile); the object is never freed.
  if (prewarm) prewarm->Stop();
}

uint32_t LauncherPrewarmStatus(NrPrewarmStatus* status) {
  NrPrewarmStatus s = {};
  s.size = sizeof(NrPrewarmStatus);
  {
    std::lock_guard lock(g_mutex);
    if (g_prewarm) g_prewarm->Status(s);
  }
  if (status && status->size >= sizeof(NrPrewarmStatus)) *status = s;
  return s.state;
}

#else  // no Vulkan game renderer in this build: the prewarm is never on

uint32_t StartLauncherPrewarm(const std::filesystem::path&, uint32_t) {
  if (REXCVAR_GET(native_launcher_prewarm)) {
    REXLOG_INFO("rexgpu-native: launcher prewarm off: this build has no Vulkan game renderer");
  }
  return NR_PREWARM_OFF;
}

void StopLauncherPrewarm() {}

uint32_t LauncherPrewarmStatus(NrPrewarmStatus* status) {
  if (status && status->size >= sizeof(NrPrewarmStatus)) {
    *status = {};
    status->size = sizeof(NrPrewarmStatus);
  }
  return NR_PREWARM_OFF;
}

#endif

}  // namespace nr
