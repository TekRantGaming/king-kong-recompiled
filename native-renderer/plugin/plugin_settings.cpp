#include "plugin/plugin_settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DECLARE(bool, native_game);
REXCVAR_DECLARE(std::string, native_shader_pack);
REXCVAR_DECLARE(std::string, native_dxc);
REXCVAR_DECLARE(bool, native_element_endian);
REXCVAR_DECLARE(bool, native_flip_front_face);
REXCVAR_DECLARE(bool, native_texture_tail_mips);
REXCVAR_DECLARE(int32_t, native_dump_frame);
REXCVAR_DECLARE(std::string, native_show_texture);
REXCVAR_DECLARE(int32_t, native_debug);
REXCVAR_DECLARE(bool, native_async_pipelines);
REXCVAR_DECLARE(bool, native_pipeline_wait);
REXCVAR_DECLARE(double, native_pipeline_wait_budget_ms);
REXCVAR_DECLARE(bool, native_dynamic_vertex_buffers);
REXCVAR_DECLARE(std::string, native_render_scale);
REXCVAR_DECLARE(double, native_shadow_scale);
REXCVAR_DECLARE(bool, native_pipeline_cache);
REXCVAR_DECLARE(std::string, native_pipeline_cache_dir);
REXCVAR_DECLARE(bool, native_vk_pipeline_cache);
REXCVAR_DECLARE(std::string, native_pipeline_list_file);

namespace nr {

std::filesystem::path ExecutableFolder() {
#if defined(_WIN32)
  wchar_t path[32768];
  DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  if (n == 0 || n >= std::size(path)) return {};
  return std::filesystem::path(std::wstring(path, n)).parent_path();
#else
  std::error_code ec;
  auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path() : p.parent_path();
#endif
}

std::string FindShaderPack(bool vulkan) {
  std::string wanted = REXCVAR_GET(native_shader_pack);
  if (!wanted.empty()) return wanted;
  const std::filesystem::path folder = ExecutableFolder();
  for (const char* name : {vulkan ? "kkshaders-spirv.pack" : "kkshaders-dxil.pack", "kkshaders.pack"}) {
    std::error_code ec;
    if (std::filesystem::exists(folder / name, ec)) return (folder / name).string();
  }
  return {};
}

GameSettings ReadGameSettings(bool vulkan) {
  GameSettings game;
  game.enabled = REXCVAR_GET(native_game);
  game.shader_pack = FindShaderPack(vulkan);
  game.dxc = REXCVAR_GET(native_dxc);
  if (game.dxc.empty()) game.dxc = ExecutableFolder().string();
  game.element_endian = REXCVAR_GET(native_element_endian);
  game.flip_front_face = REXCVAR_GET(native_flip_front_face);
  game.texture_tail_mips = REXCVAR_GET(native_texture_tail_mips);
  game.dump_frame = REXCVAR_GET(native_dump_frame);
  game.show_texture = uint32_t(std::strtoul(REXCVAR_GET(native_show_texture).c_str(), nullptr, 16));
  game.debug = uint32_t(REXCVAR_GET(native_debug));
  game.async_pipelines = REXCVAR_GET(native_async_pipelines);
  game.pipeline_wait = REXCVAR_GET(native_pipeline_wait);
  game.dynamic_vertex_buffers = REXCVAR_GET(native_dynamic_vertex_buffers);
  {
    // The launcher's texture filtering (defined by the runtime / Xenos plugin; absent: the game's own).
    const std::string aniso = rex::cvar::GetFlagByName("anisotropic_override");
    game.anisotropic_override = aniso.empty() ? -1 : std::atoi(aniso.c_str());
    // The launcher's anti-aliasing (swap_post_effect, the Xenos plugin's swap pass).
    const std::string aa = rex::cvar::GetFlagByName("swap_post_effect");
    game.fxaa = aa == "fxaa" ? 1u : (aa == "fxaa_extreme" ? 2u : 0u);
  }
  game.pipeline_wait_budget_ms = REXCVAR_GET(native_pipeline_wait_budget_ms);
  if (const char* frame_log = std::getenv("REX_DEV_FRAME_LOG")) game.frame_log = frame_log;
  game.render_scale = REXCVAR_GET(native_render_scale);
  if (game.render_scale == "auto") {
    // The scale the Xenos plugin would draw at (the runtime's rule, as kk/src/settings.cpp logs it):
    // resolution_scale when the player set it and the axis was left alone, else the axis, whose
    // default the launcher's render quality preset sets.
    auto axis = [](const char* name) {
      std::string v = rex::cvar::HasNonDefaultValue("resolution_scale") && !rex::cvar::HasNonDefaultValue(name)
                          ? rex::cvar::GetFlagByName("resolution_scale")
                          : rex::cvar::GetFlagByName(name);
      const int n = std::atoi(v.c_str());
      return n >= 1 && n <= 8 ? n : 1;
    };
    const int x = axis("draw_resolution_scale_x"), y = axis("draw_resolution_scale_y");
    game.render_scale = std::to_string(1280 * x) + "x" + std::to_string(720 * y);
    REXGPU_INFO("rexgpu-native: render scale auto: {}x{} (the draw resolution scale)", x, y);
  }
  game.shadow_scale = float(REXCVAR_GET(native_shadow_scale));
  // Ambient occlusion: the Xenos plugin's own settings (ao_mode 0 off, 1 on, 2 AO only; ao_strength
  // 1 to 3), looked up by name at every use. They are defined by rexgpu-xenos, which the launcher
  // and the game's start load first so that its cvars exist when the config and command line are read
  // (the launcher's Graphics page and F8 / F9 set them by name too). Without that plugin loaded
  // the lookup finds nothing and AO stays off.
  game.ao_settings = [] {
    AoSettings s;
    s.mode = std::clamp(std::atoi(rex::cvar::GetFlagByName("ao_mode").c_str()), 0, 2);
    s.strength = std::clamp(std::atoi(rex::cvar::GetFlagByName("ao_strength").c_str()), 1, 3);
    return s;
  };
  return game;
}

namespace {
// <title id><extension>, or game<extension> when the title is not known (a folder from the cvar).
std::string PipelineFileName(uint32_t title_id, const char* extension) {
  if (!title_id) return std::string("game") + extension;
  char name[16];
  std::snprintf(name, sizeof(name), "%08X", title_id);
  return name + std::string(extension);
}
}  // namespace

std::string PipelinePlace::records() const { return (folder / PipelineFileName(title_id, ".pipelines")).string(); }

std::string PipelinePlace::driver_cache() const { return (folder / PipelineFileName(title_id, ".vkcache")).string(); }

PipelinePlace PipelinePlaceFromCvar() {
  PipelinePlace place;
  const std::string dir = REXCVAR_GET(native_pipeline_cache_dir);
  if (!dir.empty()) place.folder = dir;
  return place;
}

PipelinePlace PipelinePlaceUnder(const std::filesystem::path& cache_root, uint32_t title_id) {
  PipelinePlace place = PipelinePlaceFromCvar();
  if (place.empty() && !cache_root.empty()) {
    place.folder = cache_root / "native-pipelines";
    place.title_id = title_id;
  }
  return place;
}

bool PipelineFilesNeedCacheRoot() {
  return REXCVAR_GET(native_pipeline_cache) || REXCVAR_GET(native_vk_pipeline_cache);
}

std::string ShippedPipelineList() {
  std::filesystem::path list = REXCVAR_GET(native_pipeline_list_file);
  if (list.empty()) list = ExecutableFolder() / "kkpipelines.list";
  return list.string();
}

}  // namespace nr
