#include "plugin/plugin_settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

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
REXCVAR_DECLARE(bool, native_render_thread);
REXCVAR_DECLARE(int32_t, native_render_thread_presents);
REXCVAR_DECLARE(std::string, native_render_scale);
REXCVAR_DECLARE(double, native_shadow_scale);
REXCVAR_DECLARE(bool, native_pipeline_cache);
REXCVAR_DECLARE(std::string, native_pipeline_cache_dir);
REXCVAR_DECLARE(bool, native_vk_pipeline_cache);
REXCVAR_DECLARE(std::string, native_pipeline_list_file);
REXCVAR_DECLARE(bool, native_stutter_stats);
REXCVAR_DECLARE(std::string, native_ao_method);
REXCVAR_DECLARE(std::string, native_ao_apply);
REXCVAR_DECLARE(std::string, native_ao_quality);
REXCVAR_DECLARE(double, native_ao_radius);
REXCVAR_DECLARE(std::string, native_ao_rules);
REXCVAR_DECLARE(std::string, native_ao_shaders);
REXCVAR_DECLARE(double, native_ao_radius_scale);
REXCVAR_DECLARE(double, native_ao_falloff);
REXCVAR_DECLARE(double, native_ao_power);
REXCVAR_DECLARE(double, native_ao_fade);
REXCVAR_DECLARE(int32_t, native_ao_dump_every);
REXCVAR_DECLARE(bool, native_ao_gpu_time);
REXCVAR_DECLARE(std::string, native_upscaler);
REXCVAR_DECLARE(std::string, native_upscaler_quality);
REXCVAR_DECLARE(std::string, native_upscaler_point);
REXCVAR_DECLARE(std::string, native_upscaler_dlss_preset);
REXCVAR_DECLARE(double, native_upscaler_sharpness);
REXCVAR_DECLARE(std::string, native_upscaler_mip_bias);
REXCVAR_DECLARE(double, native_upscaler_reactive);
REXCVAR_DECLARE(int32_t, native_upscaler_debug);
REXCVAR_DECLARE(bool, native_upscaler_gpu_time);
REXCVAR_DECLARE(std::string, native_upscaler_rules);

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
  // Only --native_shader_pack here: the user's cache and the folder beside the game are looked at once the
  // cache root is known (shader_pack_service.h, FindShaderPackForRun), the user's cache first.
  (void)vulkan;
  return REXCVAR_GET(native_shader_pack);
}

std::string AutoRenderScale() {
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
  return std::to_string(1280 * x) + "x" + std::to_string(720 * y);
}

namespace {

std::string ReadTextFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::stringstream text;
  text << f.rdbuf();
  return text.str();
}

// --native_ao_* (docs/ao.md): a value it cannot read is logged and its default kept.
void ReadAoConfig(AoConfig& ao) {
  if (!ParseAoMethod(REXCVAR_GET(native_ao_method), ao.method))
    REXGPU_WARN("rexgpu-native: native_ao_method '{}' is not legacy or gtao", REXCVAR_GET(native_ao_method));
  if (!ParseAoApply(REXCVAR_GET(native_ao_apply), ao.apply))
    REXGPU_WARN("rexgpu-native: native_ao_apply '{}' is not resolve, opaque or ambient", REXCVAR_GET(native_ao_apply));
  if (!ParseAoQuality(REXCVAR_GET(native_ao_quality), ao.quality))
    REXGPU_WARN("rexgpu-native: native_ao_quality '{}' is not full, half or auto", REXCVAR_GET(native_ao_quality));
  ao.radius = float(std::max(0.0, REXCVAR_GET(native_ao_radius)));
  ao.radius_scale = float(std::clamp(REXCVAR_GET(native_ao_radius_scale), 0.01, 100.0));
  ao.falloff = float(std::clamp(REXCVAR_GET(native_ao_falloff), 0.0, 1.0));
  ao.power = float(std::max(0.0, REXCVAR_GET(native_ao_power)));
  ao.fade = float(std::max(0.0, REXCVAR_GET(native_ao_fade)));
  ao.dump_every = uint32_t(std::max(0, REXCVAR_GET(native_ao_dump_every)));
  ao.gpu_time = REXCVAR_GET(native_ao_gpu_time);
  if (const std::string path = REXCVAR_GET(native_ao_rules); !path.empty()) {
    std::vector<std::string> errors;
    ao.rules = ParseAoRules(ReadTextFile(path), &errors);
    for (const std::string& e : errors) REXGPU_WARN("rexgpu-native: {}: {}", path, e);
    REXGPU_INFO("rexgpu-native: {} AO classifier rules from {}", ao.rules.size(), path);
  }
  if (const std::string path = REXCVAR_GET(native_ao_shaders); !path.empty()) {
    ao.ambient_shaders = ReadTextFile(path);
    if (ao.ambient_shaders.empty()) REXGPU_WARN("rexgpu-native: the AO shader list {} is empty or missing", path);
  }
  if (ao.method == AoMethod::kLegacy && ao.apply != AoApply::kResolve)
    REXGPU_WARN("rexgpu-native: native_ao_apply={} needs native_ao_method=gtao; the legacy AO uses resolve",
                AoApplyName(ao.apply));
}

// --native_upscaler* (docs/upscalers.md): a value it cannot read is logged and its default kept.
void ReadUpscaleConfig(UpscaleConfig& u) {
  if (!ParseUpscalerKind(REXCVAR_GET(native_upscaler), u.kind))
    REXGPU_WARN("rexgpu-native: native_upscaler '{}' is not off, auto, fsr3, fsr4, dlss or xess", REXCVAR_GET(native_upscaler));
  if (!ParseUpscaleQuality(REXCVAR_GET(native_upscaler_quality), u.quality))
    REXGPU_WARN("rexgpu-native: native_upscaler_quality '{}' is not native, quality, balanced, performance or "
                "ultra_performance", REXCVAR_GET(native_upscaler_quality));
  const std::string point = REXCVAR_GET(native_upscaler_point);
  if (point == "present") u.point = UpscalePoint::kPresent;
  else if (point != "post") REXGPU_WARN("rexgpu-native: native_upscaler_point '{}' is not post or present", point);
  if (!ParseDlssPreset(REXCVAR_GET(native_upscaler_dlss_preset), u.dlss_preset))
    REXGPU_WARN("rexgpu-native: native_upscaler_dlss_preset '{}' is not default or a letter",
                REXCVAR_GET(native_upscaler_dlss_preset));
  u.sharpness = float(std::min(1.0, REXCVAR_GET(native_upscaler_sharpness)));
  const std::string bias = REXCVAR_GET(native_upscaler_mip_bias);
  if (!bias.empty() && bias != "auto") {
    char* end = nullptr;
    const double v = std::strtod(bias.c_str(), &end);
    if (end && *end == 0) {
      u.mip_bias_auto = false;
      u.mip_bias = float(std::clamp(v, -4.0, 4.0));
    } else {
      REXGPU_WARN("rexgpu-native: native_upscaler_mip_bias '{}' is not auto or a number", bias);
    }
  }
  u.reactive = float(std::clamp(REXCVAR_GET(native_upscaler_reactive), 0.0, 1.0));
  u.debug = uint32_t(REXCVAR_GET(native_upscaler_debug));
  u.gpu_time = REXCVAR_GET(native_upscaler_gpu_time);
  u.rules_file = REXCVAR_GET(native_upscaler_rules);
}

}  // namespace

UpscaleConfig ReadUpscalerChoice() {
  UpscaleConfig u;
  ParseUpscalerKind(REXCVAR_GET(native_upscaler), u.kind);
  ParseUpscaleQuality(REXCVAR_GET(native_upscaler_quality), u.quality);
  return u;
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
  game.render_thread = REXCVAR_GET(native_render_thread);
  game.render_thread_presents = uint32_t(std::clamp<int32_t>(REXCVAR_GET(native_render_thread_presents), 1, 4));
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
    game.render_scale = AutoRenderScale();
    REXGPU_INFO("rexgpu-native: render scale auto: {} (the draw resolution scale)", game.render_scale);
  }
  game.shadow_scale = float(REXCVAR_GET(native_shadow_scale));
  game.stutter_stats = REXCVAR_GET(native_stutter_stats);
  // Ambient occlusion: the Xenos plugin's own settings (ao_mode 0 off, 1 on, 2 AO only; ao_strength
  // 1 to 3), looked up by name at every use. They are defined by rexgpu-xenos, which the launcher
  // and the game's start load first so that its cvars exist when the config and command line are read
  // (the launcher's Graphics page and F8 / F9 set them by name too). Without that plugin loaded
  // the lookup finds nothing and AO stays off.
  game.ao_settings = [] {
    AoSettings s;
    s.mode = std::clamp(std::atoi(rex::cvar::GetFlagByName("ao_mode").c_str()), 0, 2);
    s.strength = std::clamp(std::atoi(rex::cvar::GetFlagByName("ao_strength").c_str()), 1, 3);
    s.sample_quality = std::clamp(std::atoi(rex::cvar::GetFlagByName("native_ao_samples").c_str()), 1, 3);
    return s;
  };
  ReadAoConfig(game.ao);
  ReadUpscaleConfig(game.upscale);
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
