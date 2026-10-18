#include "hooks/kk_native_photo.h"

#include <initializer_list>

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

namespace nr::kk {

namespace {
using SetFn = uint32_t (*)(const NrPhotoSettings*);
SetFn FindSet() {
#if defined(_WIN32)
  for (const char* module : {"rexgpu-nativerd.dll", "rexgpu-native.dll", "rexgpu-natived.dll"})
    if (HMODULE m = GetModuleHandleA(module))
      if (FARPROC f = GetProcAddress(m, "nr_photo_set")) return reinterpret_cast<SetFn>(f);
#else
  for (const char* module : {"librexgpu-nativerd.so", "librexgpu-native.so", "librexgpu-natived.so"}) {
    if (void* m = dlopen(module, RTLD_NOW | RTLD_NOLOAD)) {
      void* f = dlsym(m, "nr_photo_set");
      dlclose(m);  // still loaded by the runtime
      if (f) return reinterpret_cast<SetFn>(f);
    }
  }
#endif
  return nullptr;
}
}  // namespace

bool SetPhoto(const NrPhotoSettings& settings) {
  static SetFn set = nullptr;
  if (!set) set = FindSet();
  return set && set(&settings) != 0;
}

}  // namespace nr::kk
