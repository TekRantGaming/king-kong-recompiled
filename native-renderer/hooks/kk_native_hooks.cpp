// Game-side D3D hooks for the native renderer: compiled into king_kong (add
// this file and hook_table.cpp to the game target; see the stream's doc). Each
// REX_HOOK_RAW replaces one recompiled Direct3D library function, forwards the
// call to rexgpu-native through its NrApi table (when that plugin is the one
// loaded) and then runs the game's original.
//
// All game builds load the native plugin and include these hooks. The old
// developer Direct3D tracer hooks the same functions and is no longer built.

#include <atomic>
#include <cstdlib>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "hooks/hook_table.h"
#include "backend/thread_scheduling.h"

namespace {

using GetApiFn = const NrApi* (*)();

const NrApi* LookUpApi() {
  // The launcher preloads rexgpu-native in every run of a native-enabled build
  // (for its cvars), so its export is found even when another plugin draws:
  // connect only when it is the GPU plugin.
  if (rex::cvar::GetFlagByName("gpu_plugin") != "native") {
    return nullptr;
  }
  GetApiFn get_api = nullptr;
#if defined(_WIN32)
  for (const char* name : {"rexgpu-nativerd.dll", "rexgpu-native.dll", "rexgpu-natived.dll"}) {
    if (HMODULE module = GetModuleHandleA(name)) {
      get_api = reinterpret_cast<GetApiFn>(GetProcAddress(module, "nr_get_api"));
      if (get_api) break;
    }
  }
#else
  for (const char* name :
       {"librexgpu-nativerd.so", "librexgpu-native.so", "librexgpu-natived.so"}) {
    // RTLD_NOLOAD: only if the runtime already loaded it as the GPU plugin.
    if (void* module = dlopen(name, RTLD_NOW | RTLD_NOLOAD)) {
      get_api = reinterpret_cast<GetApiFn>(dlsym(module, "nr_get_api"));
      dlclose(module);
      if (get_api) break;
    }
  }
#endif
  const NrApi* api = get_api ? get_api() : nullptr;
  if (api && !nr::hooks::IsCompatible(api)) {
    REXLOG_ERROR("native renderer: plugin API version {} (size {}), hooks expect {} (size {})",
                 api->version, api->size, NR_API_VERSION, sizeof(NrApi));
    api = nullptr;
  }
  if (api) {
    REXLOG_INFO("native renderer: D3D hooks connected to rexgpu-native ({} entry points)",
                nr::hooks::Table().size());
  }
  return api;
}

// A handler that came back with another floating-point control state (Run puts it back): the
// first few are logged with the entry, then one line every 1000.
void OnFpStateChanged(const nr::hooks::HookEntry& entry, uint32_t before, uint32_t after) {
  static std::atomic<uint64_t> count{0};
  const uint64_t n = ++count;
  if (n <= 20 || n % 1000 == 0) {
    REXLOG_WARN("native renderer: {} ({:08X}) changed MXCSR {:08X} -> {:08X}, restored ({} times)", entry.name,
                entry.address, before, after, n);
  }
}

// Looked up once, on the first hooked call (the plugin is loaded before the
// game's code runs).
const NrApi* Api() {
  static const NrApi* api = [] {
    nr::hooks::g_fp_state_changed = OnFpStateChanged;
    return LookUpApi();
  }();
  return api;
}

void ScheduleGameThreadOnce() {
  // Present identifies the game/draw thread without pinning asset-loader threads.
  thread_local const bool scheduled = [] {
    // Task 295: kk_cpu_priority for this binary's threads (the game thread here, audio).
    static const bool level = [] {
      nr::SetCpuPriorityLevel(std::atoi(rex::cvar::GetFlagByName("kk_cpu_priority").c_str()));
      return true;
    }();
    (void)level;
    nr::ScheduleCriticalThread();
    return true;
  }();
  (void)scheduled;
  // The guest may set its own thread priority later (KeSetBasePriorityThread): raise it again.
  thread_local uint32_t presents = 0;
  if (++presents % 600 == 0 && nr::CpuPriorityLevel().load(std::memory_order_relaxed) >= 1 &&
      GetThreadPriority(GetCurrentThread()) < THREAD_PRIORITY_ABOVE_NORMAL) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    REXLOG_INFO("rexgpu-native: game thread priority restored to above normal (kk_cpu_priority)");
  }
}

nr::hooks::GuestArgs ArgsFrom(const PPCContext& ctx) {
  nr::hooks::GuestArgs a;
  a.r[0] = ctx.r3.u32;
  a.r[1] = ctx.r4.u32;
  a.r[2] = ctx.r5.u32;
  a.r[3] = ctx.r6.u32;
  a.r[4] = ctx.r7.u32;
  a.r[5] = ctx.r8.u32;
  a.r[6] = ctx.r9.u32;
  a.r[7] = ctx.r10.u32;
  a.f1 = ctx.f1.f64;
  return a;
}

}  // namespace

#define NR_HOOK(ADDR)                                                                     \
  REX_EXTERN(__imp__sub_##ADDR);                                                         \
  REX_HOOK_RAW(sub_##ADDR) {                                                             \
    if constexpr (0x##ADDR == 0x821147B8) ScheduleGameThreadOnce();                       \
    static const nr::hooks::HookEntry* entry = nr::hooks::Find(0x##ADDR);                \
    nr::hooks::Run(*entry, Api(), ArgsFrom(ctx), [&] {                                \
      __imp__sub_##ADDR(ctx, base);                                                   \
      return uint32_t(ctx.r3.u32);                                                    \
    });                                                                               \
  }

// One per entry of hook_table.cpp (the test nr_hook_tests checks the count).
#define NR_HOOK_LIST(X) \
  X(82109788)           \
  X(821097F8)           \
  X(82109CB0)           \
  X(82109D20)           \
  X(82109E78)           \
  X(82109EC8)           \
  X(82109F00)           \
  X(82109F48)           \
  X(8210A1E0)           \
  X(8210A2D0)           \
  X(8210A4E8)           \
  X(8210B160)           \
  X(8210B270)           \
  X(8210B378)           \
  X(8210B540)           \
  X(8210B6E0)           \
  X(8210B750)           \
  X(8210B7A0)           \
  X(8210B7F0)           \
  X(8210BAC8)           \
  X(8210BD38)           \
  X(8210BE38)           \
  X(8210C130)           \
  X(8210C378)           \
  X(8210C6E0)           \
  X(8210CB50)           \
  X(8210CB70)           \
  X(82110300)           \
  X(82110448)           \
  X(82110640)           \
  X(821108B8)           \
  X(82110C28)           \
  X(82111CA0)           \
  X(82111D90)           \
  X(82111E68)           \
  X(821147B8)           \
  X(82115418)           \
  X(821154C8)           \
  X(82115708)           \
  X(82116178)           \
  X(82118F78)           \
  X(82120028)

NR_HOOK_LIST(NR_HOOK)

// Creation is not a bind. Notify the optional loading service after the library
// has filled in both addresses; it copies only these six words on this thread.
REX_EXTERN(__imp__sub_82118A68);
REX_HOOK_RAW(sub_82118A68) {
  __imp__sub_82118A68(ctx, base);
  const NrApi* api = Api();
  if (!ctx.r3.u32 || !api || !api->is_active(api->self)) return;
  using Notify = void (*)(const uint32_t*);
  static const auto notify = []() -> Notify {
#if defined(_WIN32)
    for (const char* name : {"rexgpu-nativerd.dll", "rexgpu-native.dll", "rexgpu-natived.dll"})
      if (HMODULE module = GetModuleHandleA(name))
        if (auto fn = GetProcAddress(module, "nr_texture_preload_hint")) return reinterpret_cast<Notify>(fn);
#else
    for (const char* name : {"librexgpu-nativerd.so", "librexgpu-native.so", "librexgpu-natived.so"}) {
      if (void* module = dlopen(name, RTLD_NOW | RTLD_NOLOAD)) {
        auto fn = reinterpret_cast<Notify>(dlsym(module, "nr_texture_preload_hint"));
        dlclose(module);
        if (fn) return fn;
      }
    }
#endif
    return nullptr;
  }();
  if (notify) {
    uint32_t words[6];
    for (unsigned i = 0; i < 6; ++i) {
      const auto* p = base + ctx.r3.u32 + 16 + i * 4;
      words[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }
    notify(words);
  }
}

// Checked at load: every hook has a table entry (Find would return null).
namespace {
#define NR_HOOK_ADDRESS(ADDR) 0x##ADDR,
constexpr uint32_t kHookedAddresses[] = {NR_HOOK_LIST(NR_HOOK_ADDRESS)};
#undef NR_HOOK_ADDRESS
const bool kTableMatches = [] {
  bool ok = std::size(kHookedAddresses) == nr::hooks::Table().size();
  for (uint32_t address : kHookedAddresses) {
    ok = ok && nr::hooks::Find(address) != nullptr;
  }
  if (!ok) {
    REXLOG_ERROR("native renderer: kk_native_hooks.cpp and hook_table.cpp disagree");
  }
  return ok;
}();
}  // namespace
