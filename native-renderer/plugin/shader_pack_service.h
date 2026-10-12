// The shader pack on the player's machine, the plugin's side (docs/shaders.md, "The pack on the
// player's machine"):
//  - the build the launcher asks for through nr_get_shader_pack_api (native_shader_pack_api.h):
//    the pack the current settings need (backend, render scale variants), built from the game's
//    Shaders/xeshaders.bin into <cache root>/native-shaders by kkshaders/pack_builder.h;
//  - for a run, which pack to open: --native_shader_pack, else the user's cache, else beside the
//    game (developer builds), else none (every shader compiled with DXC on first use).
#pragma once

#include <filesystem>
#include <string>

#include "kkshaders/pack_builder.h"

namespace nr {

// native_render_scale as the backend reads it, with "auto" resolved to the port's draw
// resolution scale (see the cvar's description).
std::string ResolvedRenderScale();

// DXC's folder or library: --native_dxc, else the folder of the running program.
std::string DxcLocation();

// The pack the current settings need: the backend native_backend picks (Vulkan whenever the
// plugin has it, as NativeGraphicsSystem::Create does for "auto"), with the render scale aware
// pixel shaders when the renderer will scale.
kkshaders::PackBuildOptions PackOptionsForSettings(const std::filesystem::path& cache_root,
                                                   const std::filesystem::path& game_dir);

// The pack for a run with this API: the user's cache (<cache root>/native-shaders), else beside
// the game. Empty when there is none. `where` says which, with the pack's stamp, for the log.
std::string FindShaderPackForRun(const std::filesystem::path& cache_root, bool vulkan, std::string* where);

}  // namespace nr
