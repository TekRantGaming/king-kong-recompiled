#include "hooks/kk_native_ui.h"

#include <cstdio>

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

#include "plugin/native_prewarm.h"
#include "plugin/native_stutter.h"
#include "plugin/native_upscaler_query.h"

namespace nr::kk {

namespace {

// King Kong's title ID: the pipeline files are named after it (555307D3.pipelines), as the game's
// start names them from the title the kernel reports.
constexpr uint32_t kTitleId = 0x555307D3u;

// An export of the native plugin if it is loaded (the launcher preloads it beside the program).
void* FindExport(const char* name) {
#if defined(_WIN32)
  for (const char* module : {"rexgpu-nativerd.dll", "rexgpu-native.dll", "rexgpu-natived.dll"}) {
    if (HMODULE m = GetModuleHandleA(module)) {
      if (FARPROC f = GetProcAddress(m, name)) return reinterpret_cast<void*>(f);
    }
  }
#else
  for (const char* module : {"librexgpu-nativerd.so", "librexgpu-native.so", "librexgpu-natived.so"}) {
    if (void* m = dlopen(module, RTLD_NOW | RTLD_NOLOAD)) {
      void* f = dlsym(m, name);
      dlclose(m);  // still loaded: the launcher's reference keeps it
      if (f) return f;
    }
  }
#endif
  return nullptr;
}

}  // namespace

void StartLauncherPrewarm(const std::filesystem::path& cache_root) {
  using Start = uint32_t (*)(const NrPrewarmRequest*);
  auto start = reinterpret_cast<Start>(FindExport("nr_prewarm_start"));
  if (!start) return;
  const std::u8string root = cache_root.u8string();
  const NrPrewarmRequest request = {sizeof(NrPrewarmRequest), NR_PREWARM_VERSION,
                                    reinterpret_cast<const char*>(root.c_str()), kTitleId};
  start(&request);
}

void SetStartupFailureHandler(int (*handler)(const char*)) {
  using Set = void (*)(int (*)(const char*));
  if (auto set = reinterpret_cast<Set>(FindExport("nr_set_startup_failure_handler"))) set(handler);
}

void SetStartupPresentedHandler(void (*handler)()) {
  using Set = void (*)(void (*)());
  if (auto set = reinterpret_cast<Set>(FindExport("nr_set_startup_presented_handler"))) set(handler);
}

void StopLauncherPrewarm() {
  using Stop = void (*)();
  if (auto stop = reinterpret_cast<Stop>(FindExport("nr_prewarm_stop"))) stop();
  // The upscaler question's device and SDKs go before this process does too.
  if (auto wait = reinterpret_cast<Stop>(FindExport("nr_upscaler_query_wait"))) wait();
}

UpscalerChoices QueryUpscalers() {
  using Query = uint32_t (*)(NrUpscalerSupport*);
  static const Query query = reinterpret_cast<Query>(FindExport("nr_upscaler_query"));
  UpscalerChoices c;
  if (!query) return c;
  NrUpscalerSupport s = {};
  s.size = sizeof(s);
  c.state = int(query(&s));
  if (c.state == NR_UPSCALER_QUERY_DONE) {
    c.fsr3 = s.fsr3 != 0;
    c.fsr4 = s.fsr4 != 0;
    c.dlss = s.dlss != 0;
    c.xess = s.xess != 0;
  }
  return c;
}

std::string StutterLine() {
  using Stats = uint32_t (*)(NrStutterStats*);
  static const Stats stats = reinterpret_cast<Stats>(FindExport("nr_stutter_stats"));
  NrStutterStats s = {};
  s.size = sizeof(s);
  if (!stats || !stats(&s)) return {};
  char text[128];
  std::snprintf(text, sizeof(text), "Pipeline stutter: %llu frames (%llu waited, %llu skipped), worst %.1f ms",
                static_cast<unsigned long long>(s.stutter_frames), static_cast<unsigned long long>(s.frames_waited),
                static_cast<unsigned long long>(s.frames_skipped), s.worst_wait_ms);
  return text;
}

}  // namespace nr::kk
