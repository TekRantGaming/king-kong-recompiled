#include "native_shaders.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

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
#include "shader_progress.h"

#include <algorithm>
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

}  // namespace

struct NativeShaderPack::Impl {
  const NrShaderPackApi* api = nullptr;
  NrShaderPackJob* job = nullptr;
  NrShaderPackProgress last{};
  bool started = false;
  bool logged = false;
  bool worked = false;  // this job has built something (not only checked)
  ShaderProgressBar bar;

  void Poll() {
    if (!job) return;
    api->poll(job, &last);
    if (last.working) worked = true;
    if (worked && !last.finished) {
      ShaderJobState s;
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
}

bool NativeShaderPack::Wanted() { return rex::cvar::GetFlagByName("gpu_plugin") == "native" && PluginApi() != nullptr; }

void NativeShaderPack::Start(const std::filesystem::path& cache_dir, const std::filesystem::path& game_dir,
                             bool force) {
  if (!Wanted() || Busy()) return;
  if (impl_->job) impl_->api->release(impl_->job);
  impl_->api = PluginApi();
  impl_->last = {};
  impl_->logged = false;
  impl_->worked = false;
  impl_->bar.Reset();
  impl_->started = true;
  // Every hardware thread but one: the launcher's window and its music keep one for themselves.
  const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
  const uint32_t threads = hardware > 1 ? hardware - 1 : 1;
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

float NativeShaderPack::Fraction() {
  if (!Busy() || !impl_->worked) return -1.0f;
  return impl_->bar.Shown();
}

bool NativeShaderPack::Working() { return Busy() && impl_->worked; }

std::string NativeShaderPack::StepText() {
  if (!Working() || impl_->last.steps <= 0 || impl_->last.step <= 0) return {};
  const std::string backend = impl_->last.backend;
  const std::string api = backend == "spirv" ? "Vulkan" : backend == "dxil" ? "DirectX 12" : backend;
  const std::string what = impl_->last.step_kind == 0 ? F("Compiling the {0} shaders", {api})
                                                      : F("Preparing the {0} pipelines", {api});
  return F("Step {0} of {1}: {2}", {std::to_string(std::min(impl_->last.step, impl_->last.steps)),
                                    std::to_string(impl_->last.steps), what});
}

std::string NativeShaderPack::StatusText() {
  if (Busy()) {
    const float f = Fraction();
    return f >= 0 ? F("Preparing {0}%", {std::to_string(int(f * 100))}) : T(std::string("Checking"));
  }
  if (Ok()) return T(std::string("Ready"));
  if (Finished() && !impl_->last.cancelled) return T(std::string("Not prepared (see the log)"));
  return T(std::string("Not prepared"));
}


}  // namespace kk
