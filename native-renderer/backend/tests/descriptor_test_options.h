#pragma once
#include <cstdlib>
#include <cstring>
#include "backend/game_renderer.h"
#include "backend/tests/vk_test_device.h"

namespace nr::test {
inline void ConfigureTestDescriptors(GameRenderer::Options& options, VulkanTestDevice* gpu,
                                     unsigned* drains = nullptr) {
  const char* mode = std::getenv("NR_TEST_DESCRIPTOR_TABLES");
  if (mode && std::strcmp(mode, "minimum") == 0) options.table_capacities = kMinimumTableCapacities;
  if (mode && std::strcmp(mode, "small") == 0) options.table_capacities = kSmallTableCapacities;
  options.drain_descriptors = [gpu, drains](nvrhi::ICommandList* cl) {
    cl->close();
    gpu->Execute(cl);
    gpu->WaitIdle();
    cl->open();
    if (drains) ++*drains;
  };
}
}  // namespace nr::test
