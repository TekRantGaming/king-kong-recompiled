// Task 266: the renderer's own device-loss report (gpu_loss.h). Only errors of the "vk::Result" category (the one
// vulkan.hpp throws, vk::SystemError) count as a device error; without a handler the caller keeps its old path.
#include "backend/gpu_loss.h"
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

  SetGpuLossHandler(nullptr);
  std::printf("gpu_loss: %d cases passed\n", cases);
  return 0;
}
