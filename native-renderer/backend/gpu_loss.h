// A lost graphics device found by the renderer's own work (task 266), not by the SDK presenter.
//
// NVRHI's Vulkan back end uses vulkan.hpp with exceptions: after VK_ERROR_DEVICE_LOST (a driver reset /
// TDR) its garbage collection, waits and submissions throw vk::SystemError on whichever renderer thread
// touched the device first. Before this, that ended in std::terminate: logged by the task 229 handler, but
// never through the plugin's device-loss path, so a loss during startup was a plain crash instead of the
// startup recovery's switch to the other native API (DECISIONS 10 Oct). The host devices and the render
// thread hand such errors here; the plugin installs the handler (NativeGraphicsSystem::OnHostGpuLoss).
// Header only: the backend library (tests) and the plugin share it.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>

namespace nr {

// Ends the program (or returns when another thread is already ending it for the same loss).
using GpuLossHandler = void (*)(const char* what);

inline std::atomic<GpuLossHandler> g_gpu_loss_handler{nullptr};

inline void SetGpuLossHandler(GpuLossHandler handler) { g_gpu_loss_handler.store(handler); }

// True for an error thrown by a failed Vulkan call (vulkan.hpp's vk::SystemError is a std::system_error of
// the category named "vk::Result"); other exceptions are bugs and keep their old path.
inline bool IsVulkanError(const std::system_error& e) {
  const char* name = e.code().category().name();
  return name && std::strcmp(name, "vk::Result") == 0;
}

// A thread that owns a device of its own (the launcher's prewarm, the setup's shader-pack job; task 268)
// handles that device's errors itself: on it, guarded calls rethrow and nothing is reported, so a failure there
// ends that work cleanly and never the game's device path. Set for a scope with OwnDeviceErrors.
inline thread_local bool t_own_device_errors = false;
class OwnDeviceErrors {
 public:
  OwnDeviceErrors() : previous_(t_own_device_errors) { t_own_device_errors = true; }
  ~OwnDeviceErrors() { t_own_device_errors = previous_; }
  OwnDeviceErrors(const OwnDeviceErrors&) = delete;
  OwnDeviceErrors& operator=(const OwnDeviceErrors&) = delete;

 private:
  bool previous_;
};

// Whether a guarded call reports (a handler is installed and this thread does not own its device).
inline bool GpuLossReporting() { return !t_own_device_errors && g_gpu_loss_handler.load() != nullptr; }

// Hands the loss to the handler. False when none is installed (or this thread owns its device): the caller
// rethrows or carries on (the old behaviour).
inline bool ReportGpuLoss(const char* what) {
  if (t_own_device_errors) return false;
  if (GpuLossHandler handler = g_gpu_loss_handler.load()) {
    handler(what ? what : "unknown");
    return true;
  }
  return false;
}

// Runs `fn`. A Vulkan error thrown inside it goes to ReportGpuLoss as "<where>: <what>" (its locks are already
// released by the unwinding); without reporting (no handler, or a device-owning thread) it is rethrown as before.
// True when `fn` finished.
template <typename F>
bool RunGuarded(const char* where, F&& fn) {
  try {
    fn();
    return true;
  } catch (const std::system_error& e) {
    if (!IsVulkanError(e) || !GpuLossReporting()) throw;
    const std::string what = std::string(where) + ": " + e.what();
    ReportGpuLoss(what.c_str());
    return false;
  }
}

#if defined(KK_DEV_TOOLS)
// Task 268 dev hook (developer builds only): KK_DEV_GPU_LOSS_AFTER=<frames> makes the host device's frame
// submission number <frames> fail as a lost device: on Vulkan thrown the way vulkan.hpp throws it (a
// std::system_error of the "vk::Result" category, VK_ERROR_DEVICE_LOST = -4) through the guarded path, on
// D3D12 reported as a removed device. 0 or unset: off. The startup recovery takes a failure in the first
// frames (about 30 with the launcher hand-over); later ones end the game with the logged abort.
class DevVkResultCategory final : public std::error_category {
 public:
  const char* name() const noexcept override { return "vk::Result"; }
  std::string message(int) const override { return "ErrorDeviceLost (KK_DEV_GPU_LOSS_AFTER)"; }
};
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable : 4996)  // getenv: read once at the first frame, never stored
#endif
inline uint64_t DevGpuLossAfter() {
  static const uint64_t frames = [] {
    const char* v = std::getenv("KK_DEV_GPU_LOSS_AFTER");
    return v ? uint64_t(std::strtoull(v, nullptr, 10)) : uint64_t(0);
  }();
  return frames;
}
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif
// True exactly once, on the frame submission the variable names.
inline bool DevGpuLossNow(std::atomic<uint64_t>& frames) {
  const uint64_t after = DevGpuLossAfter();
  return after && frames.fetch_add(1, std::memory_order_relaxed) + 1 == after;
}
[[noreturn]] inline void DevThrowVulkanDeviceLost() {
  static const DevVkResultCategory category;
  throw std::system_error(std::error_code(-4, category), "developer injection: queue submit");
}
#endif

// An effect built on a worker (std::async; task 268): a Vulkan error inside the build (out of video memory while
// making its resources) ends that build with an empty result, so the effect stays off as when it cannot be made,
// instead of a rethrow at future::get on the render thread. `error` gets the text. Other exceptions unchanged.
template <typename F>
auto BuildOrEmpty(F&& fn, std::string* error) -> decltype(fn()) {
  try {
    return fn();
  } catch (const std::system_error& e) {
    if (!IsVulkanError(e)) throw;
    if (error) *error = e.what();
    return decltype(fn()){};
  }
}

}  // namespace nr
