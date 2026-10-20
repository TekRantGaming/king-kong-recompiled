#include "dev_chapter_prepare.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "plugin/native_benchmark_stats.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace kk {
namespace {
std::atomic<bool> stopped{false};
std::once_flag once;
std::thread sampler;

void* Export(const char* name) {
#if defined(_WIN32)
  const auto module = GetModuleHandleW(L"rexgpu-native.dll");
  return module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
#else
  void* module = dlopen("librexgpu-native.so", RTLD_NOW | RTLD_NOLOAD);
  void* symbol = module ? dlsym(module, name) : nullptr;
  if (module) dlclose(module);
  return symbol;
#endif
}
}

void DevStartPreparationTrace() {
  const char* path = std::getenv("KK_DEV_PREPARE_TRACE");
  if (!path || !*path) return;
  std::call_once(once, [path] {
    using Read = uint32_t (*)(NrBenchmarkStats*);
    using Enable = void (*)(uint32_t);
    const auto read = reinterpret_cast<Read>(Export("nr_benchmark_stats"));
    const auto enable = reinterpret_cast<Enable>(Export("nr_benchmark_enable"));
    if (!read || !enable) return;
    FILE* file = std::fopen(path, "w");
    if (!file) return;
    enable(1);
    std::fprintf(file, "unix_s,frame,loading,waits,skipped,textures,upload_bytes,texture_ms,prepare_ms,jobs\n");
    sampler = std::thread([read, file] {
      uint64_t previous = ~uint64_t(0);
      while (!stopped.load(std::memory_order_relaxed)) {
        NrBenchmarkStats s;
        if (read(&s) && s.frame != previous) {
          previous = s.frame;
          const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
          std::fprintf(file, "%.6f,%llu,%u,%llu,%llu,%llu,%llu,%.6f,%.6f,%u\n", now,
              static_cast<unsigned long long>(s.frame), s.resource_loading,
              static_cast<unsigned long long>(s.pipeline_waits), static_cast<unsigned long long>(s.draws_skipped),
              static_cast<unsigned long long>(s.texture_uploads), static_cast<unsigned long long>(s.upload_bytes),
              s.texture_ms, s.prepare_ms, s.worker_jobs);
          std::fflush(file);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      std::fclose(file);
    });
  });
}

void DevStopPreparationTrace() {
  stopped.store(true, std::memory_order_relaxed);
  if (sampler.joinable()) sampler.join();
}
}
