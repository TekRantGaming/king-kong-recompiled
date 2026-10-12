#include "plugin/shader_pack_service.h"
#include "plugin/plugin_settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <thread>

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
#include "kkshaders/translator.h"
#include "plugin/native_shader_pack_api.h"

REXCVAR_DECLARE(std::string, native_backend);
REXCVAR_DECLARE(std::string, native_dxc);
REXCVAR_DECLARE(std::string, native_render_scale);
REXCVAR_DECLARE(double, native_shadow_scale);

namespace nr {

namespace {

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
  const RenderScale scale = ParseRenderScale(ResolvedRenderScale(), 1280, 720);
  const float shadow = float(REXCVAR_GET(native_shadow_scale));
  o.scaled = REXCVAR_GET(native_render_scale) == "auto" || (scale.valid && !scale.unit()) ||
             (shadow > 0.0f && shadow != 1.0f);
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
  kkshaders::PackBuildResult result;
};

namespace {

NrShaderPackJob* StartJob(const char* cache_dir, const char* game_dir, uint32_t threads, int low_priority) {
  auto* job = new NrShaderPackJob;
  job->options = nr::PackOptionsForSettings(nr::FromUtf8(cache_dir), nr::FromUtf8(game_dir));
  job->options.threads = threads;
  job->options.lowPriority = low_priority != 0;
  REXLOG_INFO("rexgpu-native: shader pack: checking {} ({}{})", nr::ToUtf8(kkshaders::packPath(job->options)),
              kkshaders::packBackendName(job->options.backend), job->options.scaled ? ", render scale variants" : "");
  job->thread = std::thread([job] {
    kkshaders::PackBuildResult r = kkshaders::buildPack(job->options, &job->progress);
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
  *out = NrShaderPackProgress{};
  out->phase = job->progress.phase;
  out->done = job->progress.done;
  out->total = job->progress.total;
  out->failed = job->progress.failed;
  nr::Copy(out->backend, sizeof(out->backend), kkshaders::packBackendName(job->options.backend));
  nr::Copy(out->pack, sizeof(out->pack), nr::ToUtf8(kkshaders::packPath(job->options)));
  std::lock_guard lock(job->mutex);
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
