// The launcher's prewarm in the plugin (native_prewarm.h is its C interface): the game's known
// pipelines made on a Vulkan device of the plugin's own while the launcher is open
// (backend/pipeline_prewarm.h does the work). docs/backend.md, "Prewarm from the launcher".
#pragma once

#include <cstdint>
#include <filesystem>

#include "plugin/native_prewarm.h"

namespace nr {

// Starts it (once a process) when --native_launcher_prewarm is on, the GPU plugin is "native" and
// the game would draw with Vulkan; returns the state (NR_PREWARM_OFF when it does nothing).
uint32_t StartLauncherPrewarm(const std::filesystem::path& cache_root, uint32_t title_id);
// Stops it after the pipelines being made, saves the driver's pipeline cache and destroys its device;
// returns when that is done. The game's SetupGuestGpu calls it before making its own device.
void StopLauncherPrewarm();
uint32_t LauncherPrewarmStatus(NrPrewarmStatus* status);

}  // namespace nr
