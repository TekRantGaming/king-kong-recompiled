#include "native_shaders.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#if defined(KK_NATIVE_RENDERER)
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
#endif

#include "launcher_text.h"

namespace kk {

using text::F;
using text::T;

#if defined(KK_NATIVE_RENDERER)

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

  void Poll() {
    if (!job) return;
    api->poll(job, &last);
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

void NativeShaderPack::Start(const std::filesystem::path& cache_dir, const std::filesystem::path& game_dir) {
  if (!Wanted() || Busy()) return;
  if (impl_->job) impl_->api->release(impl_->job);
  impl_->api = PluginApi();
  impl_->last = {};
  impl_->logged = false;
  impl_->started = true;
  impl_->job = impl_->api->start(Utf8(cache_dir).c_str(), Utf8(game_dir).c_str(), 0, 1);
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
  impl_->Poll();
  if (!Busy() || impl_->last.total == 0) return -1.0f;
  return float(impl_->last.done) / float(impl_->last.total);
}

uint32_t NativeShaderPack::Done() {
  impl_->Poll();
  return impl_->last.done;
}

uint32_t NativeShaderPack::Total() {
  impl_->Poll();
  return impl_->last.total;
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

#else  // the native renderer is not built: nothing to prepare

struct NativeShaderPack::Impl {};
NativeShaderPack::NativeShaderPack() : impl_(std::make_unique<Impl>()) {}
NativeShaderPack::~NativeShaderPack() = default;
bool NativeShaderPack::Wanted() { return false; }
void NativeShaderPack::Start(const std::filesystem::path&, const std::filesystem::path&) {}
void NativeShaderPack::Cancel() {}
bool NativeShaderPack::Busy() { return false; }
bool NativeShaderPack::Finished() { return false; }
bool NativeShaderPack::Ok() { return false; }
float NativeShaderPack::Fraction() { return -1.0f; }
uint32_t NativeShaderPack::Done() { return 0; }
uint32_t NativeShaderPack::Total() { return 0; }
std::string NativeShaderPack::StatusText() { return {}; }

#endif

}  // namespace kk
