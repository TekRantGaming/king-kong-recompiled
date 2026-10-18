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
  // Task 210: the pipeline step is complete when all it could queue was made; records waiting for shaders the
  // pack does not have cannot be made at setup and must not make it incomplete.
  if (!nr::SetupPipelinesComplete(true, 9894, 9894, 0)) return 7;   // Beta 9: 47 waiting, all queued made
  if (nr::SetupPipelinesComplete(false, 9894, 9894, 0)) return 8;   // not done yet
  if (nr::SetupPipelinesComplete(true, 9894, 9893, 0)) return 9;    // one queued pipeline not made
  if (nr::SetupPipelinesComplete(true, 9894, 9894, 1)) return 10;   // one failed
  if (nr::SetupPipelinesComplete(true, 9894, 9000, 894)) return 11; // failures are never complete
  std::puts("Setup API availability, selection retry and plan invalidation: PASS");
}
