#include "plugin/shader_pack_service.h"
#include "plugin/plugin_settings.h"
#include "plugin/builtin_shaders.h"
#include "plugin/setup_prewarm_availability.h"
#include "plugin/gpu_preference.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <fstream>
#include <sstream>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "backend/render_scale.h"
#include "backend/host_device.h"
#include "backend/pipeline_prewarm.h"
#include "backend/driver_pipeline_cache.h"
#include "backend/thread_scheduling.h"
#include <rex/kernel/xboxkrnl/video.h>
#if NR_HAS_VULKAN
#include <rex/ui/vulkan/provider.h>
#endif
#if NR_HAS_D3D12
#include <rex/ui/d3d12/d3d12_provider.h>
#endif
#include "kkshaders/translator.h"
#include "plugin/native_shader_pack_api.h"

REXCVAR_DECLARE(std::string, native_backend);
REXCVAR_DECLARE(std::string, native_dxc);
REXCVAR_DECLARE(std::string, native_render_scale);
REXCVAR_DECLARE(double, native_shadow_scale);
REXCVAR_DECLARE(std::string, native_water_reflections);
REXCVAR_DECLARE(bool, native_pipeline_list);
REXCVAR_DECLARE(bool, native_pipeline_cache);
REXCVAR_DECLARE(bool, native_vk_pipeline_cache);
REXCVAR_DECLARE(std::string, native_failed_backends);

namespace nr {

namespace {

std::filesystem::path ProgramFolder();

std::string WindowsDriverStamp() {
#if defined(_WIN32) && NR_HAS_D3D12
  // DX12 has no application VkPipelineCache to check. Record installed adapters
  // and driver versions without creating a device on every warm launcher start.
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};
  std::ostringstream out;
  for (UINT i = 0;; ++i) {
    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(i, &adapter) != S_OK) break;
    DXGI_ADAPTER_DESC1 desc{};
    LARGE_INTEGER version{};
    if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
        SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version)))
      out << desc.VendorId << ':' << desc.DeviceId << ':' << desc.SubSysId << ':'
          << desc.Revision << ':' << version.QuadPart << '\n';
    adapter->Release();
  }
  factory->Release();
  return out.str();
#else
  return {};
#endif
}

std::string PrewarmStamp(const kkshaders::PackStamp& stamp, const nr::PipelinePrewarm::Settings& settings,
                         const std::string& player_records, const std::string& drivers) {
  std::ostringstream out;
  // 3 (task 144): the game's own layouts (live settings: the scale and lighting constants in every
  // pipeline), with and without the ambient-only AO; 2 prepared layouts the game never used.
  out << stamp.toText() << "pipeline layouts 3\n" << drivers;
  // These choices change shader replacements and must invalidate preparation when the player changes them.
  out << settings.flip_front_face << ' ' << int(settings.lighting.shadows) << ' '
      << int(settings.lighting.water) << ' ' << int(settings.lighting.volumetrics) << ' ' << settings.ao.ambient()
      << '\n';
  std::error_code ec;
  // A renderer update or a changed pipeline list invalidates preparation even if the translator did not change.
#if defined(__linux__)
  const auto plugin = nr::ProgramFolder() / "librexgpu-native.so";
#else
  const auto plugin = nr::ProgramFolder() / "rexgpu-native.dll";
#endif
  out << std::filesystem::last_write_time(plugin, ec).time_since_epoch().count() << '\n';
  for (const auto& list : settings.lists) {
    // Newly recorded pipelines were already compiled by the game. Their append must not trigger
    // another full preparation at the next Play; the shipped list and renderer updates still do.
    if (list == player_records) continue;
#if defined(__linux__)
    // An AppImage mounts the program under a new /tmp/.mount_<random> folder at every start: the
    // shipped list beside the program is named by its file name, or the stamp never matched and
    // the pipeline step ran again at every launcher start and Play.
    if (std::filesystem::path(list).parent_path() == nr::ProgramFolder()) {
      out << "<program>/" << std::filesystem::path(list).filename().string();
    } else {
      out << list;
    }
#else
    out << list;
#endif
    out << '\n' << std::filesystem::file_size(list, ec) << '\n'
        << std::filesystem::last_write_time(list, ec).time_since_epoch().count() << '\n';
  }
  return out.str();
}

std::filesystem::path ProgramFolder() {
#if defined(_WIN32)
  wchar_t path[32768];
  DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  if (n == 0 || n >= std::size(path)) return {};
  return std::filesystem::path(std::wstring(path, n)).parent_path();
#else
  std::error_code ec;
  auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path() : p.parent_path();
#endif
}

std::filesystem::path FromUtf8(const char* text) {
  if (!text) return {};
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text)));
}

std::string ToUtf8(const std::filesystem::path& path) {
  const std::u8string s = path.u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

void Copy(char* to, size_t size, const std::string& from) {
  if (!size) return;
  const size_t n = std::min(size - 1, from.size());
  std::memcpy(to, from.data(), n);
  to[n] = 0;
}

}  // namespace

std::string ResolvedRenderScale() {
  const std::string scale = REXCVAR_GET(native_render_scale);
  return scale == "auto" ? AutoRenderScale() : scale;
}

std::string DxcLocation() {
  std::string dxc = REXCVAR_GET(native_dxc);
  return dxc.empty() ? ProgramFolder().string() : dxc;
}

kkshaders::PackBuildOptions PackOptionsForSettings(const std::filesystem::path& cache_root,
                                                   const std::filesystem::path& game_dir) {
  kkshaders::PackBuildOptions o;
  o.database = game_dir / "Shaders" / "xeshaders.bin";
  o.additionalContainers = ReadBuiltinShaderContainers(game_dir / "default.xex");
  o.folder = cache_root / kkshaders::kPackFolderName;
#if NR_HAS_VULKAN
  o.backend = REXCVAR_GET(native_backend) == "d3d12" ? kkshaders::PackBackend::Dxil : kkshaders::PackBackend::Spirv;
#else
  o.backend = kkshaders::PackBackend::Dxil;
#endif
  // As Backend::Initialize decides it (the frame image's size does not matter for the decision).
  // "auto" follows the launcher's render quality, which the player can change at any time and which
  // the game only applies when it starts (the launcher still sees the default scale 1 when it checks
  // the pack): with "auto" the pack always carries the render scale variants, so it fits every run.
  // Setup and Play use the same complete set, even before the launcher applies settings.
  o.scaled = true;
  o.motion = true;
  // The lighting options' water reflections: their hooked water shaders (docs/lighting.md).
  o.lighting = true;
  o.dxc = DxcLocation();
  return o;
}

std::string FindShaderPackForRun(const std::filesystem::path& cache_root, bool vulkan, std::string* where) {
  const kkshaders::PackBackend backend = vulkan ? kkshaders::PackBackend::Spirv : kkshaders::PackBackend::Dxil;
  std::error_code ec;
  if (!cache_root.empty()) {
    const std::filesystem::path p = cache_root / kkshaders::kPackFolderName / kkshaders::packFileName(backend);
    if (std::filesystem::is_regular_file(p, ec)) {
      kkshaders::PackStamp stamp;
      if (where) {
        if (!kkshaders::readPackStamp(p, stamp)) {
          *where = "the user's cache (no stamp: an unfinished build?)";
        } else {
          *where = "the user's cache (" + std::to_string(stamp.shaders) + " shaders, " + stamp.dxc +
                   (stamp.scaled ? ", with the render scale variants" : "") +
                   (stamp.translator != kkshaders::translatorHash() ? ", another translator: refused" : "") + ")";
        }
      }
      return p.string();
    }
  }
  // Developer builds: a pack from kkshaders db-build --split beside the game.
  const std::filesystem::path folder = ProgramFolder();
  for (const char* name : {kkshaders::packFileName(backend), "kkshaders.pack"}) {
    if (std::filesystem::is_regular_file(folder / name, ec)) {
      if (where) *where = "beside the game (a developer's pack)";
      return (folder / name).string();
    }
  }
  if (where) *where = "none";
  return {};
}

}  // namespace nr

// ------------------------------------------------------------------ the launcher's interface ---

struct NrShaderPackJob {
  kkshaders::PackBuildOptions options;
  kkshaders::PackBuildProgress progress;
  std::thread thread;
  std::mutex mutex;
  bool finished = false;
  bool force = false;
  kkshaders::PackBuildResult result;
  nr::PipelinePrewarm::Settings prewarm[2];
  std::string driver_cache_path;
  std::string player_records;
  bool need_driver_cache = false;
  std::string failed_backends;  // --native_failed_backends when the job started
  // The steps (NrShaderPackProgress): per API its shaders, then its pipelines in two passes.
  std::atomic<int> step{0}, steps{0}, step_kind{0}, pass{0};
  std::atomic<bool> preparing{false};  // the pipelines are being prepared (not only checked)
};

namespace {

NrShaderPackJob* StartJob(const char* cache_dir, const char* game_dir, uint32_t threads, int low_priority, int force) {
  auto* job = new NrShaderPackJob;
  job->force = force != 0;
  job->options = nr::PackOptionsForSettings(nr::FromUtf8(cache_dir), nr::FromUtf8(game_dir));
  const unsigned budget = nr::ProcessSetupWorkerCount();
  job->options.threads = threads ? std::min(threads, budget) : budget;
  job->options.lowPriority = low_priority != 0;
  // Capture settings on the UI thread, before the worker starts. Prepare both APIs during
  // the existing shader screen, including scale and motion pipelines, before Play can proceed.
  const auto place = nr::PipelinePlaceUnder(nr::FromUtf8(cache_dir), 0x555307D3u);
  job->driver_cache_path = place.driver_cache();
  job->player_records = place.records();
  job->need_driver_cache = REXCVAR_GET(native_vk_pipeline_cache);
  job->failed_backends = REXCVAR_GET(native_failed_backends);
  rex::system::X_VIDEO_MODE mode{};
  rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
  for (unsigned i = 0; i < 2; ++i) {
    const auto game = nr::ReadGameSettings(i == 0);
    const auto scale = nr::ResolveRenderScale(game.render_scale, game.shadow_scale, game.debug,
        std::max<uint32_t>(1, mode.display_width), std::max<uint32_t>(1, mode.display_height));
    auto& settings = job->prewarm[i];
    settings.dxc = job->options.dxc;
    settings.render_scale_x = scale.scale.x;
    settings.render_scale_y = scale.scale.y;
    settings.shadow_scale = scale.shadow_scale;
    settings.shaders_aware = scale.shaders_aware;
    settings.flip_front_face = game.flip_front_face;
    settings.lighting = game.lighting;
    settings.upscale = game.upscale;
    settings.ao = game.ao;
    if (REXCVAR_GET(native_pipeline_cache)) settings.lists.push_back(place.records());
    if (REXCVAR_GET(native_pipeline_list)) settings.lists.push_back(nr::ShippedPipelineList());
  }
  REXLOG_INFO("rexgpu-native: shader pack: {} {} ({}{}), {} workers (process affinity, one CPU free)",
              job->force ? "rebuilding (asked for)" : "checking",
              nr::ToUtf8(kkshaders::packPath(job->options)), kkshaders::packBackendName(job->options.backend),
              job->options.scaled ? ", render scale variants" : "", job->options.threads);
  job->thread = std::thread([job] {
    const auto selected_backend = job->options.backend;
    kkshaders::PackBuildResult r;
    bool current = true;
    bool pipelines_ok = true;
    double seconds = 0;
    uint32_t compiled = 0;
    std::string warnings;
    const std::string drivers = nr::WindowsDriverStamp();
    std::vector<kkshaders::PackBackend> backends;
    for (auto backend : {kkshaders::PackBackend::Spirv, kkshaders::PackBackend::Dxil}) {
#if defined(__linux__)
      if (backend == kkshaders::PackBackend::Dxil) continue;
#endif
#if !NR_HAS_VULKAN
      if (backend == kkshaders::PackBackend::Spirv) continue;
#endif
      // An API startup recovery found failing on this PC: no pack, and no headless device for it.
      if (job->failed_backends.find(backend == kkshaders::PackBackend::Spirv ? "vulkan" : "d3d12") !=
          std::string::npos) {
        REXLOG_INFO("rexgpu-native: {} shader pack skipped (that API failed to start here)",
                    kkshaders::packBackendName(backend));
        continue;
      }
      backends.push_back(backend);
    }
    job->steps = int(backends.size()) * 2;
    for (size_t b = 0; b < backends.size(); ++b) {
      const auto backend = backends[b];
      job->progress.phase = kkshaders::PackBuildProgress::kStarting;
      job->step_kind = 0;
      job->pass = 0;
      job->step = int(b) * 2 + 1;
      auto options = job->options;
      options.backend = backend;
      {
        std::lock_guard lock(job->mutex);
        job->options.backend = backend;
      }
      r = kkshaders::buildPack(options, &job->progress, job->force);
      current = current && r.upToDate;
      seconds += r.seconds;
      compiled += r.compiled;
      if (!r.error.empty()) warnings += std::string(kkshaders::packBackendName(backend)) + ": " + r.error + "; ";
      REXLOG_INFO("rexgpu-native: {} shader pack: {} shaders, {:.1f} s, {}",
                  kkshaders::packBackendName(backend), r.stamp.shaders, r.seconds,
                  r.ok ? (r.upToDate ? "current" : "built") : r.error);
      if (!r.ok || job->progress.cancel) break;
      // The pipelines' step (ends at once when they are ready).
      job->progress.done = 0;
      job->progress.total = 0;
      job->step_kind = 1;
      job->step = int(b) * 2 + 2;
      auto marker = r.pack;
      marker += ".pipelines-ready";
      const auto& plan = job->prewarm[backend == kkshaders::PackBackend::Spirv ? 0 : 1];
      const auto expected = nr::PrewarmStamp(r.stamp, plan, job->player_records, drivers);
      const nr::SetupPrewarmAvailability availability(r.pack, expected, backend == selected_backend);
      if (availability.Skip()) {
        REXLOG_INFO("rexgpu-native: setup {} pipeline device previously unavailable; retry when selected",
                    kkshaders::packBackendName(backend));
        continue;
      }
      std::ifstream prepared(marker, std::ios::binary);
      const std::string previous((std::istreambuf_iterator<char>(prepared)), {});
      prepared.close();
      std::error_code marker_error;
      // A driver cache that could not be written is not asked for again at every start: the attempt
      // for this plan is remembered beside the pack.
      auto driver_attempt = r.pack;
      driver_attempt += ".driver-cache-attempted";
      std::ifstream attempted_file(driver_attempt, std::ios::binary);
      const std::string attempted((std::istreambuf_iterator<char>(attempted_file)), {});
      attempted_file.close();
      const bool driver_ready = !job->need_driver_cache || backend != kkshaders::PackBackend::Spirv ||
          std::filesystem::is_regular_file(job->driver_cache_path, marker_error) ||
          (previous == expected && attempted == expected);
      std::unique_ptr<rex::ui::GraphicsProvider> provider;
      std::unique_ptr<nr::HostDevice> host;
      bool driver_changed = false;
#if NR_HAS_VULKAN
      // The same usability check on Windows and Linux: existence alone does not
      // prove this GPU/driver can load the cache. Rebuild before Play if it cannot.
      // On Windows the stamp already holds every adapter's driver version (WindowsDriverStamp), so a
      // driver change shows there: no device is made at every launcher start and Play just to read the
      // cache (task 144). Elsewhere the device's own check stays.
      if (r.upToDate && previous == expected && driver_ready && job->need_driver_cache && drivers.empty() &&
          backend == kkshaders::PackBackend::Spirv &&
          std::filesystem::is_regular_file(job->driver_cache_path, marker_error)) {
        nr::PreferHighPerformanceGpu(nr::GpuApi::kVulkan);  // the game's GPU, not adapter 0 (task 203)
        nr::PrepareVulkanDeviceRequests();  // the graphics pipeline library (task 183)
        provider = rex::ui::vulkan::VulkanProvider::Create(true, false);
        if (provider) host = nr::CreateVulkanHostDevice(*provider);
        if (host && host->pipeline_cache()) {
          std::string note;
          const auto load = host->pipeline_cache()->LoadFile(job->driver_cache_path, &note);
          if (load != nr::DriverPipelineCache::Load::kLoaded) {
            driver_changed = true;
            REXLOG_INFO("rexgpu-native: setup: the Vulkan driver pipeline cache is {} ({}): preparing the pipelines again",
                        nr::LoadResultName(load), note);
          }
        }
        if (!driver_changed) {
          host.reset();
          provider.reset();
        }
      }
#endif
      if (!r.upToDate || previous != expected || !driver_ready || driver_changed || availability.Retry()) {
        const auto start = std::chrono::steady_clock::now();
#if NR_HAS_VULKAN
        if (backend == kkshaders::PackBackend::Spirv && !host) {
          nr::PreferHighPerformanceGpu(nr::GpuApi::kVulkan);  // the game's GPU, not adapter 0 (task 203)
          nr::PrepareVulkanDeviceRequests();  // the graphics pipeline library (task 183)
          provider = rex::ui::vulkan::VulkanProvider::Create(true, false);
          if (provider) host = nr::CreateVulkanHostDevice(*provider);
        }
#endif
#if NR_HAS_D3D12
        if (backend == kkshaders::PackBackend::Dxil) {
          // The SDK's default is the first adapter: the integrated GPU on hybrid laptops, where
          // Intel's driver crashed preparing these pipelines (Beta 7/8 reports, task 203).
          nr::PreferHighPerformanceGpu(nr::GpuApi::kD3D12);
          provider = rex::ui::d3d12::D3D12Provider::Create();
          if (provider) host = nr::CreateD3D12HostDevice(*provider);
        }
#endif
        if (host) {
          bool completed = true;
          job->preparing = true;
          // The game's pipelines, as Backend::Initialize sets the renderer up (task 144): live settings
          // (scale and lighting constants in every layout, scale-aware shaders) and an upscaler's motion
          // variants (a run without one makes a subset of them). The look, the upscaler, the render scale
          // and AO on or off do not change these layouts, so whatever the player picks after the setup is
          // prepared now and Play makes nothing new. Only the ambient-only AO (--native_ao_apply=ambient,
          // not a player setting) has a layout of its own: the settings' AO is used, and the stamp has it.
          {
            job->pass = 0;
            job->progress.done = 0;
            job->progress.total = 0;
            auto settings = job->prewarm[backend == kkshaders::PackBackend::Spirv ? 0 : 1];
            settings.render_scale_x = settings.render_scale_y = 1.0f;
            settings.shadow_scale = 0.0f;
            settings.shaders_aware = true;
            settings.live_layout = true;
            settings.upscale.kind = nr::UpscalerKind::kBilinear;
            settings.upscale.quality = nr::UpscaleQuality::kQuality;
            settings.shader_pack = r.pack.string();
            if (host->pipeline_cache()) {
              settings.driver_cache = host->pipeline_cache();
              settings.driver_cache_path = job->driver_cache_path;
            }
            nr::PipelinePrewarm prewarm(host->device());
            settings.tessellation_supported = host->tessellation_supported();
            settings.table_capacities = host->table_capacities();
            if (prewarm.Start(settings)) {
              job->progress.phase = kkshaders::PackBuildProgress::kCompiling;
              while (!job->progress.cancel) {
                const auto p = prewarm.progress();
                job->progress.total = uint32_t(p.queued);
                job->progress.done = uint32_t(p.made);
                if (p.done) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
              }
              const auto p = prewarm.progress();
              // Which records name shaders the pack does not have (a list to correct; task 171).
              for (const auto& w : prewarm.waiting_records()) {
                REXLOG_WARN("rexgpu-native: setup {} pipeline waiting for shaders: vs {:016X}/{:016X} ps {:016X}/{:016X} flags {}",
                            kkshaders::packBackendName(backend), w.vs_hash, w.vs_input, w.ps_hash, w.ps_input, w.flags);
              }
              prewarm.Stop();
              REXLOG_INFO("rexgpu-native: setup {} pipeline prewarm: {} listed, {} queued, {} finished, "
                          "{} failed, {} waiting for shaders",
                          kkshaders::packBackendName(backend), p.listed, p.queued, p.made, p.failed, p.waiting);
              // The ambient-only AO's own pixel shaders are not in the pack: the game makes those pipelines
              // at first use, as it always did. Everything else must be made.
              completed = completed && p.done && p.made == p.queued && !p.failed &&
                          (!p.waiting || settings.ao.ambient());
            } else {
              completed = false;
            }
            }
          host->WaitForIdle();
          job->preparing = false;
          if (completed && !job->progress.cancel) {
            std::ofstream ready(marker, std::ios::binary | std::ios::trunc);
            ready << expected;
            availability.Prepared();
          } else if (!job->progress.cancel) {
            pipelines_ok = false;
            warnings += std::string(kkshaders::packBackendName(backend)) + ": pipeline preparation incomplete; ";
          }
          // The device goes first: the driver cache is saved with it.
          host.reset();
          provider.reset();
          std::error_code driver_error;
          if (completed && !job->progress.cancel && job->need_driver_cache &&
              backend == kkshaders::PackBackend::Spirv &&
              !std::filesystem::is_regular_file(job->driver_cache_path, driver_error)) {
            REXLOG_WARN("rexgpu-native: setup could not write the Vulkan driver pipeline cache; not retried "
                        "until the shaders or settings change");
            std::ofstream attempt(driver_attempt, std::ios::binary | std::ios::trunc);
            attempt << expected;
          }
        } else {
          pipelines_ok = availability.DeviceUnavailable() && pipelines_ok;
          warnings += std::string(kkshaders::packBackendName(backend)) + ": pipeline device unavailable; ";
          REXLOG_WARN("rexgpu-native: setup {} pipeline prewarm unavailable", kkshaders::packBackendName(backend));
        }
        seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      }
    }
    r.ok = r.ok && pipelines_ok;
    r.upToDate = r.ok && current;
    r.seconds = seconds;
    r.compiled = compiled;
    if (job->progress.cancel) {
      r.ok = r.upToDate = false;
      r.cancelled = true;
      r.error = "cancelled";
    } else if (!warnings.empty()) {
      r.error = warnings;
    }
    if (r.upToDate) {
      REXLOG_INFO("rexgpu-native: shader pack is current ({} shaders)", r.stamp.shaders);
    } else if (r.ok) {
      REXLOG_INFO("rexgpu-native: shader pack built: {} shaders on {} threads in {:.1f} s, {} MB{}{}", r.compiled, r.threads,
                  r.seconds, r.bytes >> 20, r.error.empty() ? "" : "; ", r.error);
    } else {
      REXLOG_WARN("rexgpu-native: shader pack not built: {}", r.error);
    }
    std::lock_guard lock(job->mutex);
    job->result = std::move(r);
    job->finished = true;
  });
  return job;
}

void PollJob(NrShaderPackJob* job, NrShaderPackProgress* out) {
  if (!job || !out) return;
  std::lock_guard lock(job->mutex);
  *out = NrShaderPackProgress{};
  out->phase = job->progress.phase;
  out->done = job->progress.done;
  out->total = job->progress.total;
  out->failed = job->progress.failed;
  out->step = job->step;
  out->steps = job->steps;
  out->step_kind = job->step_kind;
  out->pass = job->pass;
  out->passes = 1;  // one pass of the game's layouts (task 144)
  // Shaders: working once the pack turned out stale (reading the database, compiling, writing).
  const int phase = job->progress.phase;
  out->working = job->step_kind == 1 ? int(job->preparing.load())
                                     : int(phase >= kkshaders::PackBuildProgress::kReading &&
                                           phase <= kkshaders::PackBuildProgress::kWriting);
  nr::Copy(out->backend, sizeof(out->backend), kkshaders::packBackendName(job->options.backend));
  nr::Copy(out->pack, sizeof(out->pack), nr::ToUtf8(kkshaders::packPath(job->options)));
  if (!job->finished) return;
  const kkshaders::PackBuildResult& r = job->result;
  out->finished = 1;
  out->ok = r.ok;
  out->up_to_date = r.upToDate;
  out->cancelled = r.cancelled;
  out->seconds = r.seconds;
  nr::Copy(out->message, sizeof(out->message),
           r.ok && r.error.empty() ? (r.upToDate ? std::string("current") : "built " + std::to_string(r.compiled) + " shaders")
                                   : r.error);
}

void CancelJob(NrShaderPackJob* job) {
  if (job) job->progress.cancel = true;
}

void ReleaseJob(NrShaderPackJob* job) {
  if (!job) return;
  if (job->thread.joinable()) job->thread.join();
  delete job;
}

const NrShaderPackApi kApi = {NR_SHADER_PACK_API_VERSION, sizeof(NrShaderPackApi), StartJob, PollJob, CancelJob, ReleaseJob};

}  // namespace

extern "C" REX_GPU_PLUGIN_EXPORT const NrShaderPackApi* nr_get_shader_pack_api(void) { return &kApi; }
