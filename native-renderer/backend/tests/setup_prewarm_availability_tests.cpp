#include "plugin/setup_prewarm_availability.h"
#include <chrono>
#include <cstdio>

int main() {
  const auto root = std::filesystem::current_path() / ("setup-availability-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(root);
  // Exercise both API orderings: Vulkan selected, then D3D12 selected.
  for (const char* name : {"spirv.pack", "dxil.pack"}) {
    const auto pack = root / name;
    nr::SetupPrewarmAvailability secondary(pack, "plan-driver-1", false);
    if (secondary.Skip() || secondary.Retry() || !secondary.DeviceUnavailable()) return 1;
    nr::SetupPrewarmAvailability remembered(pack, "plan-driver-1", false);
    if (!remembered.Skip() || remembered.Retry()) return 2;
    nr::SetupPrewarmAvailability switched(pack, "plan-driver-1", true);
    if (switched.Skip() || !switched.Retry() || switched.DeviceUnavailable()) return 3;
    // A selected failure remains a failure on repeated attempts.
    nr::SetupPrewarmAvailability retry(pack, "plan-driver-1", true);
    if (!retry.Retry() || retry.DeviceUnavailable()) return 4;
    retry.Prepared();
    nr::SetupPrewarmAvailability ready(pack, "plan-driver-1", false);
    if (ready.Skip() || ready.Retry()) return 5;
    ready.DeviceUnavailable();
    nr::SetupPrewarmAvailability changed(pack, "plan-driver-2", false);
    if (changed.Skip() || changed.Retry()) return 6;
  }
  std::puts("Setup API availability, selection retry and plan invalidation: PASS");
}
