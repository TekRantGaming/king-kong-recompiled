// The game's side of the native renderer's launcher prewarm and stutter counter: compiled into
// king_kong (kk/CMakeLists.txt, with KK_NATIVE_RENDERER) like kk_native_hooks.cpp. Each function
// looks for the plugin the launcher preloaded and does nothing when it is not there; the plugin
// decides from its own options whether to do anything (docs/backend.md, "Prewarm from the
// launcher", "Stutter counter").
#pragma once

namespace nr::kk {
void SetStartupFailureHandler(int (*handler)(const char*));
}

#include <filesystem>
#include <string>

namespace nr::kk {

// The launcher opened: with --native_launcher_prewarm the plugin starts making the game's pipelines
// (`cache_root` is the one the game will run with).
void StartLauncherPrewarm(const std::filesystem::path& cache_root);
// The launcher quits or restarts the program without playing: stop it (Play needs nothing, the
// game's start stops it).
void StopLauncherPrewarm();
// The launcher's Display > Upscaling menu: which temporal upscalers this PC runs with the backend the
// game will use (plugin/native_upscaler_query.h). The first call starts the question on a thread of the
// plugin's; state 2 once the answer is in, 1 meanwhile, 0 or 3 when there is none (no native plugin,
// no Direct3D 12, no device): then none is offered.
struct UpscalerChoices {
  int state = 0;
  bool fsr3 = false, fsr4 = false, dlss = false, xess = false;
};
UpscalerChoices QueryUpscalers();
// The overlay's line under the frame counter; empty when --native_stutter_stats is off or the native
// renderer is not drawing.
std::string StutterLine();

}  // namespace nr::kk
