// The plugin's cvars as the backend's settings, and where the pipeline files are: shared by the
// game's start (NativeGraphicsSystem) and the launcher's prewarm (launcher_prewarm.h), which must
// make exactly the game's pipelines.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "backend/backend.h"

namespace nr {

// The folder of the running executable (the game's).
std::filesystem::path ExecutableFolder();
// native_shader_pack (the user's cache and the game's folder come later: FindShaderPackForRun).
std::string FindShaderPack(bool vulkan);
// native_render_scale=auto's scale, "WxH": the port's draw resolution scale (one rule for the run, the
// launcher's prewarm and the shader pack build).
std::string AutoRenderScale();
// The shadow maps' scale: native_shadow_scale when set, else the one native_shadow_quality asks for
// (0: like the render scale). The run, the launcher's prewarm and the shader pack take it from here.
float EffectiveShadowScale();
// The game renderer's settings from the cvars (and the port's settings the Xenos plugin defines).
GameSettings ReadGameSettings(bool vulkan);

// Where the pipeline files are: the folder of native_pipeline_cache_dir (files named game.*), else
// <cache root>/native-pipelines (files named after the title, 555307D3.*).
struct PipelinePlace {
  std::filesystem::path folder;
  uint32_t title_id = 0;
  bool empty() const { return folder.empty(); }
  std::string records() const;       // <title>.pipelines: the player's pipeline cache file
  std::string driver_cache() const;  // <title>.vkcache: the driver's pipeline cache
};
// native_pipeline_cache_dir alone (empty when it is not set).
PipelinePlace PipelinePlaceFromCvar();
// Under the game's cache root (native_pipeline_cache_dir still wins).
PipelinePlace PipelinePlaceUnder(const std::filesystem::path& cache_root, uint32_t title_id);
// Whether a file of ours lives under the cache root (native_pipeline_cache, native_vk_pipeline_cache).
bool PipelineFilesNeedCacheRoot();
// The shipped list: native_pipeline_list_file, else kkpipelines.list beside the game.
std::string ShippedPipelineList();

}  // namespace nr
