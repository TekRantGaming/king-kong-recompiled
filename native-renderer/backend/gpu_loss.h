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
#include <cstring>
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

// Hands the loss to the handler. False when none is installed: the caller rethrows (the old behaviour).
inline bool ReportGpuLoss(const char* what) {
  if (GpuLossHandler handler = g_gpu_loss_handler.load()) {
    handler(what ? what : "unknown");
    return true;
  }
  return false;
}

}  // namespace nr
