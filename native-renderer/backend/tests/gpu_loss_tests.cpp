// Task 266: the renderer's own device-loss report (gpu_loss.h). Only errors of the "vk::Result" category (the one
// vulkan.hpp throws, vk::SystemError) count as a device error; without a handler the caller keeps its old path.
#include "backend/gpu_loss.h"
#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <system_error>

static int cases = 0;
static void Check(bool condition, const char* what) {
  ++cases;
  if (!condition) {
    std::fprintf(stderr, "case %d failed: %s\n", cases, what);
    std::exit(1);
  }
}

namespace {
// The same name vulkan.hpp's errorCategoryImpl gives (VULKAN_HPP_NAMESPACE_STRING "::Result").
class FakeVkCategory final : public std::error_category {
 public:
  const char* name() const noexcept override { return "vk::Result"; }
  std::string message(int) const override { return "ErrorDeviceLost"; }
};
const FakeVkCategory kVk;

std::string g_seen;
int g_calls = 0;
void Record(const char* what) {
  ++g_calls;
  g_seen = what;
}
}  // namespace

int main() {
  using namespace nr;
  const std::system_error vk_lost(std::error_code(-4, kVk), "vk::Device::getSemaphoreCounterValue");
  const std::system_error generic(std::make_error_code(std::errc::not_enough_memory), "other");
  const std::system_error system(std::error_code(5, std::system_category()), "system");
  Check(IsVulkanError(vk_lost), "a vk::Result error is a Vulkan error");
  Check(!IsVulkanError(generic), "a generic error is not");
  Check(!IsVulkanError(system), "a system error is not");

  // No handler: nothing reported, the caller rethrows.
  SetGpuLossHandler(nullptr);
  Check(!ReportGpuLoss("x"), "no handler: not reported");
  Check(g_calls == 0, "no handler: nothing called");

  SetGpuLossHandler(&Record);
  Check(ReportGpuLoss("Vulkan submission: ErrorDeviceLost"), "handler: reported");
  Check(g_calls == 1 && g_seen == "Vulkan submission: ErrorDeviceLost", "handler gets the text");
  Check(ReportGpuLoss(nullptr) && g_seen == "unknown", "null text becomes 'unknown'");

  // The guard pattern the hosts and the render thread use: rethrow without a handler, report with one.
  auto guarded = [](bool with_handler) {
    SetGpuLossHandler(with_handler ? &Record : nullptr);
    std::string what;
    try {
      try {
        throw std::system_error(std::error_code(-4, kVk), "submit");
      } catch (const std::system_error& e) {
        if (!IsVulkanError(e) || !g_gpu_loss_handler.load()) throw;
        what = std::string("render thread: ") + e.what();
      }
    } catch (const std::system_error&) {
      return std::string("rethrown");
    }
    ReportGpuLoss(what.c_str());
    return what;
  };
  Check(guarded(false) == "rethrown", "guard without handler rethrows");
  const int before = g_calls;
  Check(guarded(true).rfind("render thread: submit", 0) == 0 && g_calls == before + 1, "guard with handler reports once");

  // Task 268: RunGuarded reports a Vulkan error once (after the scope's locks), rethrows others.
  {
    const int calls = g_calls;
    static std::atomic<bool> held{false};
    static bool locked_in_report = false;
    struct Lock {
      Lock() { held = true; }
      ~Lock() { held = false; }
    };
    SetGpuLossHandler([](const char* what) {
      Record(what);
      if (held.load()) locked_in_report = true;
    });
    const bool finished = RunGuarded("Vulkan submission", [] {
      Lock lock;
      throw std::system_error(std::error_code(-4, kVk), "submit");
    });
    Check(!finished && g_calls == calls + 1, "RunGuarded reports a Vulkan error");
    Check(g_seen.rfind("Vulkan submission: submit", 0) == 0, "RunGuarded names where");
    Check(!locked_in_report, "RunGuarded reports after the scope's lock is released");
    bool other = false;
    try {
      RunGuarded("x", [] { throw std::system_error(std::make_error_code(std::errc::io_error), "io"); });
    } catch (const std::system_error&) {
      other = true;
    }
    Check(other && g_calls == calls + 1, "RunGuarded rethrows a non-Vulkan error");
    Check(RunGuarded("x", [] {}), "RunGuarded: nothing thrown, finished");
  }
  // A device-owning thread: nothing reported, the error is rethrown to its own handler.
  SetGpuLossHandler(&Record);
  {
    const int calls = g_calls;
    bool rethrown = false;
    {
      OwnDeviceErrors own;
      Check(!GpuLossReporting(), "own-device thread: not reporting");
      Check(!ReportGpuLoss("x"), "own-device thread: ReportGpuLoss refuses");
      try {
        RunGuarded("prewarm", [] { throw std::system_error(std::error_code(-4, kVk), "lost"); });
      } catch (const std::system_error&) {
        rethrown = true;
      }
    }
    Check(rethrown && g_calls == calls, "own-device thread: rethrown, handler not called");
    Check(GpuLossReporting(), "own-device scope restored");
    std::thread other_thread([] { OwnDeviceErrors own; });
    other_thread.join();
    Check(GpuLossReporting(), "another thread's scope does not touch this one");
  }
  // BuildOrEmpty: a Vulkan error makes an empty result and keeps the text; others propagate.
  {
    std::string error;
    auto ok = BuildOrEmpty([] { return std::make_unique<int>(7); }, &error);
    Check(ok && *ok == 7 && error.empty(), "BuildOrEmpty: normal build unchanged");
    auto empty = BuildOrEmpty([]() -> std::unique_ptr<int> { throw std::system_error(std::error_code(-2, kVk), "oom"); }, &error);
    Check(!empty && error.rfind("oom", 0) == 0, "BuildOrEmpty: Vulkan error -> empty + text");
    bool propagated = false;
    try {
      (void)BuildOrEmpty([]() -> std::unique_ptr<int> { throw std::runtime_error("bug"); }, nullptr);
    } catch (const std::runtime_error&) {
      propagated = true;
    }
    Check(propagated, "BuildOrEmpty: other exceptions propagate");
  }
#if defined(KK_DEV_TOOLS)
  {
    // The dev hook's thrown error is recognised as a Vulkan error; DevGpuLossNow fires once at the frame named.
    bool seen = false;
    try {
      DevThrowVulkanDeviceLost();
    } catch (const std::system_error& e) {
      seen = IsVulkanError(e) && e.code().value() == -4;
    }
    Check(seen, "dev injection looks like vk::DeviceLostError");
    std::atomic<uint64_t> frames{0};
    int fired = 0;
    for (int i = 0; i < 10; ++i) fired += DevGpuLossNow(frames, "vulkan") ? 1 : 0;
    Check(fired == (DevGpuLossAfter() && DevGpuLossAfter() <= 10 ? 1 : 0), "dev hook fires once at its frame");
  }
#endif

  SetGpuLossHandler(nullptr);
  std::printf("gpu_loss: %d cases passed\n", cases);
  return 0;
}
