#include "native_shaders.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <fmt/format.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include "../../native-renderer/plugin/native_shader_pack_api.h"

#include "launcher_text.h"
#include "settings.h"
#include "shader_progress.h"
#include "startup_recovery.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <thread>

namespace kk {

using text::F;
using text::T;


namespace {

// The builder's interface in the native renderer's plugin, which PreloadGpuPlugin loaded (null
// when it was not built or not found).
const NrShaderPackApi* PluginApi() {
  static const NrShaderPackApi* api = []() -> const NrShaderPackApi* {
    using GetApi = const NrShaderPackApi* (*)();
    void* symbol = nullptr;
    for (const char* postfix : {"rd", "", "d"}) {
#if defined(_WIN32)
      const std::wstring file = L"rexgpu-native" + std::wstring(postfix, postfix + std::char_traits<char>::length(postfix)) + L".dll";
      if (HMODULE module = GetModuleHandleW(file.c_str()))
        symbol = reinterpret_cast<void*>(GetProcAddress(module, "nr_get_shader_pack_api"));
#else
      const std::string file = std::string("librexgpu-native") + postfix + ".so";
      const auto dir = rex::filesystem::GetExecutableFolder();
      for (const auto& path : {dir / file, dir / ".." / "lib" / file}) {
        if (void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD)) {
          symbol = dlsym(module, "nr_get_shader_pack_api");
          dlclose(module);  // NOLOAD took a reference of its own
          if (symbol) break;
        }
      }
#endif
      if (symbol) break;
    }
    const NrShaderPackApi* a = symbol ? reinterpret_cast<GetApi>(symbol)() : nullptr;
    if (a && (a->version != NR_SHADER_PACK_API_VERSION || a->size < sizeof(NrShaderPackApi))) {
      REXLOG_WARN("KK: the native renderer's shader pack interface is version {}, the launcher's {}", a->version,
                  NR_SHADER_PACK_API_VERSION);
      a = nullptr;
    }
    return a;
  }();
  return api;
}

std::string Utf8(const std::filesystem::path& p) {
  const std::u8string s = p.u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

#if defined(_WIN32)
// The process's own CPUs, while a limit is on. In the environment too, so a process this one starts during the job
// (it inherits the limit) can lift it at its start.
constexpr const char* kFullCpusVariable = "KK_SHADER_FULL_CPUS";

// Process affinity holds every thread of the job to the limit, the driver's own included, and the renderer sizes
// its pipeline workers by it. Windows only: elsewhere the limit reaches the shader builders alone.
void LimitShaderCpus(unsigned limit, int crashes) {
  DWORD_PTR allowed = 0, system = 0;
  const char* full = std::getenv(kFullCpusVariable);
  if ((full && *full) || !GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system) || !allowed) return;
  const auto limited = DWORD_PTR(LimitedAffinity(allowed, limit));
  if (limited == allowed) return;
  _putenv_s(kFullCpusVariable, fmt::format("{:x}", uint64_t(allowed)).c_str());
  if (!SetProcessAffinityMask(GetCurrentProcess(), limited)) {
    _putenv_s(kFullCpusVariable, "");
    return;
  }
  REXLOG_INFO("KK: shader preparation limited to {} CPU threads ({} ended unexpectedly in a row)", limit, crashes);
}
#endif

}  // namespace

void LiftShaderCpuLimit() {
#if defined(_WIN32)
  const char* full = std::getenv(kFullCpusVariable);
  if (!full || !*full) return;
  if (const uint64_t mask = std::strtoull(full, nullptr, 16)) {
    const bool ok = SetProcessAffinityMask(GetCurrentProcess(), DWORD_PTR(mask)) != 0;
    REXLOG_INFO("KK: shader preparation's CPU limit lifted, all CPU threads back ({})", ok ? "ok" : "failed");
  }
  _putenv_s(kFullCpusVariable, "");
#endif
}

struct NativeShaderPack::Impl {
  const NrShaderPackApi* api = nullptr;
  NrShaderPackJob* job = nullptr;
  NrShaderPackProgress last{};
  bool started = false;
  bool logged = false;
  bool taken = false;   // TakeFinished() reported this job
  bool worked = false;  // this job has built something (not only checked)
  ShaderProgressBar bar;

  void Poll() {
    if (!job) return;
    api->poll(job, &last);
    if (last.working) worked = true;
    if (!last.finished && last.steps > 0) {
      ShaderJobState s;
      s.working = last.working != 0;
      s.step = last.step;
      s.steps = last.steps;
      s.step_kind = last.step_kind;
      s.phase = last.phase;
      s.pass = last.pass;
      s.passes = last.passes;
      s.done = last.done;
      s.total = last.total;
      bar.Update(s);
    }
    if (last.finished && !logged) {
      logged = true;
      LiftShaderCpuLimit();
      if (last.ok)
        REXLOG_INFO("KK: native renderer shaders {} ({} pack, {:.1f} s): {}", last.up_to_date ? "ready" : "prepared",
                    last.backend, last.seconds, last.message);
      else
        REXLOG_WARN("KK: native renderer shaders not prepared: {}", last.message);
    }
  }
};

NativeShaderPack::NativeShaderPack() : impl_(std::make_unique<Impl>()) {}

NativeShaderPack::~NativeShaderPack() {
  if (impl_->job) {
    impl_->api->cancel(impl_->job);
    impl_->api->release(impl_->job);
  }
  LiftShaderCpuLimit();
}

bool NativeShaderPack::Wanted() { return rex::cvar::GetFlagByName("gpu_plugin") == "native" && PluginApi() != nullptr; }

void NativeShaderPack::Start(const std::filesystem::path& cache_dir, const std::filesystem::path& game_dir,
                             bool force) {
  if (!Wanted() || Busy()) return;
  if (impl_->job) impl_->api->release(impl_->job);
  impl_->api = PluginApi();
  impl_->last = {};
  impl_->logged = false;
  impl_->taken = false;
  impl_->worked = false;
  impl_->bar.Reset();
  impl_->started = true;
  int crashes = startup::ShaderCrashes();
#if defined(KK_DEV_TOOLS)
  // KK_DEV_SHADER_CRASHES=<n>: as if the job had ended unexpectedly n times in a row.
  if (const char* v = std::getenv("KK_DEV_SHADER_CRASHES")) crashes = std::atoi(v);
#endif
  // The plugin caps this request to process affinity, leaving one CPU for the UI/music.
  const bool limited = !REXCVAR_GET(kk_shader_full_cpu);
  const uint32_t threads = limited ? ShaderThreadLimit(crashes) : 0;
#if defined(_WIN32)
  if (limited) LimitShaderCpus(threads, crashes);
#endif
  if (!limited) REXLOG_INFO("KK: shader preparation: the player removed the CPU limit (kk_shader_full_cpu)");
  impl_->job = impl_->api->start(Utf8(cache_dir).c_str(), Utf8(game_dir).c_str(), threads, 1, force ? 1 : 0);
}

void NativeShaderPack::Cancel() {
  if (impl_->job) impl_->api->cancel(impl_->job);
}

bool NativeShaderPack::Busy() {
  impl_->Poll();
  return impl_->job && !impl_->last.finished;
}

bool NativeShaderPack::Finished() {
  impl_->Poll();
  return impl_->job && impl_->last.finished;
}

bool NativeShaderPack::Ok() { return Finished() && impl_->last.ok; }

bool NativeShaderPack::Cancelled() { return Finished() && impl_->last.cancelled; }

ShaderJobEnd NativeShaderPack::End() {
  ShaderJobEnd e;
  e.finished = Finished();
  if (!e.finished) return e;
  e.cancelled = impl_->last.cancelled != 0;
  e.packs_needed = impl_->last.packs_needed;
  e.packs_ready = impl_->last.packs_ready;
  return e;
}

bool NativeShaderPack::TakeFinished() {
  if (!Finished() || impl_->taken) return false;
  impl_->taken = true;
  return true;
}

float NativeShaderPack::Fraction() {
  if (!Busy() || !impl_->worked) return -1.0f;
  return impl_->bar.Shown();
}

bool NativeShaderPack::Working() { return Busy() && impl_->worked; }

float NativeShaderPack::DrawnFraction(double now) {
  if (!Busy() || !impl_->worked) return -1.0f;
  return impl_->bar.Drawn(now);
}

std::string NativeShaderPack::ProgressLabel() {
  // One label for the whole job, whichever API's shaders or pipelines are being prepared (owner, 16 Oct).
  return T(std::string(Working() ? "Preparing shaders" : "Checking"));
}

ShaderHomeStatus NativeShaderPack::Status(bool prepared) {
  return HomeStatus(Busy(), Working(), prepared, Finished(), Cancelled());
}

std::string NativeShaderPack::StatusText(bool prepared) {
  switch (Status(prepared)) {
    case ShaderHomeStatus::kPreparing: {
      const float f = Fraction();
      return f >= 0 ? F("Preparing {0}%", {std::to_string(int(f * 100))}) : T(std::string("Checking"));
    }
    case ShaderHomeStatus::kChecking: return T(std::string("Checking"));
    case ShaderHomeStatus::kReady: return T(std::string("Ready"));
    case ShaderHomeStatus::kFailed: return T(std::string("Not prepared (see the log)"));
    case ShaderHomeStatus::kNotPrepared: break;
  }
  return T(std::string("Not prepared"));
}


}  // namespace kk
