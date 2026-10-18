#include "hardware_recommendation.h"
#include "settings.h"
#include "graphics_presets.h"
#include "ultrawide_logic.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>

// This console test links the real settings implementation and cvar registry,
// with OS detection replaced by controlled profiles. No game/device is started.
REXCVAR_DEFINE_STRING(native_upscaler, "off", "Test", "Test");
REXCVAR_DEFINE_STRING(native_backend, "auto", "Test", "Test");
REXCVAR_DEFINE_STRING(gpu_plugin, "native", "Test", "Test");
REXCVAR_DEFINE_STRING(native_upscaler_quality, "quality", "Test", "Test");
REXCVAR_DEFINE_STRING(native_upscaler_output, "", "Test", "Test");
REXCVAR_DEFINE_STRING(native_frame_size, "", "Test", "Test");
REXCVAR_DEFINE_STRING(native_display_aspect, "", "Test", "Test");
REXCVAR_DEFINE_STRING(native_ao_quality, "auto", "Test", "Test");
REXCVAR_DEFINE_INT32(native_ao_samples, 2, "Test", "Test");
REXCVAR_DEFINE_INT32(ao_mode, 0, "Test", "Test");
REXCVAR_DEFINE_STRING(present_effect, "fsr", "Test", "Test");
REXCVAR_DEFINE_INT32(resolution_scale, 1, "Test", "Test");
REXCVAR_DEFINE_INT32(draw_resolution_scale_x, 1, "Test", "Test");
REXCVAR_DEFINE_INT32(draw_resolution_scale_y, 1, "Test", "Test");
REXCVAR_DEFINE_STRING(native_gi, "off", "Test", "Test");
REXCVAR_DEFINE_STRING(native_gi_distance, "medium", "Test", "Test");
REXCVAR_DEFINE_STRING(swap_post_effect, "none", "Test", "Test");
REXCVAR_DEFINE_INT32(anisotropic_override, -1, "Test", "Test");
REXCVAR_DEFINE_INT32(ao_strength, 1, "Test", "Test");

namespace kk {
HardwareProfile fake;
GpuInfo DetectGpu() { return fake.gpu; }
HardwareProfile DetectHardware() { return fake; }
}
namespace {
int checks = 0;
void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
void Equals(const char* name, const char* expected) {
  const std::string actual = rex::cvar::GetFlagByName(name);
  if (actual != expected) std::cerr << name << " = '" << actual << "', expected '" << expected << "'" << '\n';
  Check(actual == expected, name);
}
}
int main() {
  using namespace kk;
  for (const char* legacy : {"classic", "xenos"}) {
    rex::cvar::SetFlagByName("kk_renderer", legacy);
    rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    rex::cvar::SetFlagByName("native_backend", "d3d12");
    Check(NormalizeRendererSettings(), "legacy renderer migrated");
    Equals("gpu_plugin", "native"); Equals("kk_renderer", "native"); Equals("native_backend", "auto");
    Check(!NormalizeRendererSettings(), "migration is idempotent");
  }
  rex::cvar::SetFlagByName("native_backend", "classic");
  Check(NormalizeRendererSettings(), "legacy API migrated");
  Equals("native_backend", "auto");
  rex::cvar::SetFlagByName("native_backend", "d3d12");
  Check(!NormalizeRendererSettings(), "native API preserved");
  Equals("native_backend", "d3d12");
  rex::cvar::SetFlagByName("native_backend", "auto");
  constexpr uint64_t GiB = uint64_t(1) << 30;
  struct Case { GpuVendor vendor; const char* model; unsigned vram, cores, ram; bool deck; size_t tier; };
  const Case cases[] = {
    {GpuVendor::kNvidia, "GT 1030", 2, 4, 8, false, 0},
    {GpuVendor::kNvidia, "GTX 1080 Ti", 11, 8, 32, false, 0},
    {GpuVendor::kNvidia, "GTX 1650", 4, 4, 8, false, 0},
    {GpuVendor::kNvidia, "RTX 3050", 8, 6, 16, false, 0},
    {GpuVendor::kNvidia, "RTX 3060", 12, 4, 16, false, 1},
    {GpuVendor::kNvidia, "RTX 4060", 8, 6, 16, false, 1},
    {GpuVendor::kNvidia, "RTX 4070 Ti", 12, 8, 32, false, 2},
    {GpuVendor::kNvidia, "RTX 5090", 32, 16, 64, false, 2},
    {GpuVendor::kAmd, "RX 580", 8, 6, 16, false, 0},
    {GpuVendor::kAmd, "RX 6500 XT", 4, 4, 8, false, 0},
    {GpuVendor::kAmd, "RX 6600", 8, 6, 16, false, 1},
    {GpuVendor::kAmd, "RX 6700 XT", 12, 6, 16, false, 2},
    {GpuVendor::kAmd, "RX 7900 XTX", 24, 8, 32, false, 2},
    {GpuVendor::kIntel, "Arc A380", 6, 4, 16, false, 0},
    {GpuVendor::kIntel, "Arc A750", 8, 6, 16, false, 1},
    {GpuVendor::kIntel, "Intel Arc B580", 12, 6, 16, false, 2},
    {GpuVendor::kIntel, "UHD 630", 0, 6, 8, false, 0},
    {GpuVendor::kIntel, "Iris Xe", 1, 4, 8, false, 0},
    {GpuVendor::kAmd, "Radeon 780M", 0, 8, 16, false, 0},
    {GpuVendor::kAmd, "AMD Custom GPU 0405", 1, 4, 16, true, 0},
    {GpuVendor::kAmd, "AMD Custom GPU 0932", 16, 4, 16, true, 0},
    {GpuVendor::kUnknown, "", 0, 0, 0, false, 0},
    {GpuVendor::kNvidia, "Future unknown GPU", 24, 8, 32, false, 1},
    {GpuVendor::kNvidia, "RTX 4070", 12, 2, 16, false, 0},
    {GpuVendor::kNvidia, "RTX 4070", 12, 8, 6, false, 0},
    {GpuVendor::kNvidia, "RTX 4070", 12, 8, 8, false, 1},
  };
  for (const auto& c : cases) {
    fake = {{c.vendor, c.model, c.vram * GiB}, c.cores, c.ram * GiB, 1920, 1080, 144, c.deck};
    Check(HardwareTierIndex(fake) == c.tier, c.model);
  }
  for (const auto& tier : kHardwareTiers) {
    Check(std::string(RecommendedRenderQuality(tier, 3840, 2160)) ==
          (std::string(tier.id) == "high" ? "quality" : "performance"), "4K policy");
    Check(std::string(RecommendedRenderQuality(tier, 1920, 1080)) == tier.render_quality, "1080p policy");
  }
  // Every possible supported-SDK combination, all vendors, both backend choices.
  for (int mask = 0; mask < 16; ++mask) for (auto vendor : {GpuVendor::kNvidia, GpuVendor::kAmd, GpuVendor::kIntel, GpuVendor::kUnknown}) {
    const UpscalerSupport s{true, bool(mask & 1), bool(mask & 2), bool(mask & 4), bool(mask & 8)};
    for (bool dx : {false, true}) {
      const char* expected = vendor == GpuVendor::kNvidia && s.dlss ? "dlss" :
          vendor == GpuVendor::kIntel && s.xess ? "xess" :
          vendor == GpuVendor::kAmd && dx && s.fsr4 ? "fsr4" : s.fsr3 ? "fsr3" : "off";
      Check(std::string(RecommendedUpscaler(vendor, s, dx)) == expected, "SDK feature fallback");
    }
  }
  Check(std::string(RecommendedUpscaler(GpuVendor::kNvidia, {}, false)) == "dlss", "NVIDIA pending");
  Check(std::string(RecommendedUpscaler(GpuVendor::kAmd, {}, false)) == "fsr3", "AMD pending");
  Check(std::string(RecommendedUpscaler(GpuVendor::kIntel, {}, false)) == "xess", "Intel pending");
  for (bool explicit_value : {false, true}) for (bool custom : {false, true}) for (bool original : {false, true})
    Check(PreserveHardwareSetting(explicit_value, custom, original) == (explicit_value || custom || original), "preservation policy");

  const auto dir = std::filesystem::current_path() / ("hardware-test-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);
  const auto config = dir / "king_kong.toml";
  ApplyPortDefaults();
  Equals("native_gi", "medium");
  fake = {{GpuVendor::kNvidia, "RTX 4070 Ti", 12 * GiB}, 8, 32 * GiB, 3840, 2160, 120, false};
  Check(rex::cvar::SetFlagByName("gpu_plugin", "native"), "test native renderer selected");
  InitializeHardwareRecommendation(config, 3840, 2160);
  Equals("kk_hardware_tier", "high");
  Equals("native_ao_samples", "3");
  Equals("native_ao_quality", "auto");
  Equals("kk_frame_rate", "30");
  Equals("native_backend", "auto");
  Equals("native_upscaler", "dlss");
  Equals("native_gi", "high");
  Equals("native_gi_distance", "far");
  SetOfferedUpscalers("fsr3");
  Equals("native_upscaler", "fsr3");
  Check(SaveSettings(config), "save first-launch marker");
  fake.gpu.dedicated_memory = 2 * GiB;
  InitializeHardwareRecommendation(config, 3840, 2160);
  Equals("kk_hardware_tier", "high"); // hardware change alone does not retier
  ApplyHardwareRecommendation(true, 1920, 1080);
  Equals("kk_hardware_tier", "low");
  Equals("kk_render_quality", "performance");
  Equals("native_ao_quality", "half");
  Equals("native_ao_samples", "1");
  Equals("ao_mode", "0");
  Equals("native_gi", "low");
  Equals("native_gi_distance", "near");

  // Explicit choice equal to default must remain explicit after saving and loading.
  rex::cvar::SetFlagByName("native_ao_quality", "half");
  rex::cvar::SetFlagByName("kk_render_quality", "performance");
  rex::cvar::SetFlagByName("native_upscaler_quality", "balanced");
  rex::cvar::SetFlagByName("native_upscaler", "off");
  rex::cvar::SetFlagByName("kk_frame_rate", "144");
  rex::cvar::SetFlagByName("native_backend", "d3d12");
  rex::cvar::SetFlagByName("native_gi", "low");
  rex::cvar::SetFlagByName("native_gi_distance", "near");
  Check(SaveSettings(config), "save explicit choices");
  rex::cvar::ResetAllToDefaults();
  rex::cvar::LoadConfig(config);
  fake.gpu.dedicated_memory = 12 * GiB;
  ApplyHardwareRecommendation(true, 1920, 1080);
  Equals("kk_render_quality", "performance");
  Equals("native_ao_quality", "half");
  Equals("native_upscaler_quality", "balanced");
  Equals("native_upscaler", "off");
  Equals("kk_frame_rate", "144");
  Equals("native_backend", "d3d12");
  Equals("native_gi", "low"); // explicit choice equal to the default survives a new recommendation
  Equals("native_gi_distance", "near"); // default-valued manual distance also survives save/reload and retiering
  for (auto source : {rex::cvar::Source::kConfig, rex::cvar::Source::kEnvironment,
                     rex::cvar::Source::kCommandLine, rex::cvar::Source::kRuntime}) {
    for (auto& e : rex::cvar::GetRegistry()) if (e.name == "native_ao_samples") { e.setter("2"); e.source = source; }
    ApplyHardwareRecommendation(true, 1920, 1080);
    Equals("native_ao_samples", "2");
  }
  rex::cvar::ResetAllToDefaults();
  rex::cvar::SetFlagByName("resolution_scale", "2");
  ApplyHardwareRecommendation(true, 1920, 1080);
  Equals("kk_render_quality", "custom");
  ApplyRenderPreset(1920, 1080);
  Equals("resolution_scale", "2");
  // The upscaler's output is the chosen resolution, never above it (DECISIONS 16 Oct).
  auto size = [](std::pair<int, int> s, int w, int h) { return s.first == w && s.second == h; };
  Check(size(UpscalerOutputSize(1920, 1080), 1920, 1080), "4K screen set to 1080p: 1080p out");
  Check(size(UpscalerOutputSize(2560, 1440), 2560, 1440), "4K screen set to 1440p: 1440p out");
  Check(size(UpscalerOutputSize(3840, 2160), 3840, 2160), "4K desktop: 4K out");
  Check(size(UpscalerOutputSize(1600, 900), 1600, 900), "900p");
  Check(size(UpscalerOutputSize(1366, 768), 1365, 768), "1366x768: the 16:9 inside");
  Check(size(UpscalerOutputSize(1920, 1200), 1920, 1080), "16:10 window: letterboxed 16:9");
  Check(size(UpscalerOutputSize(2560, 1080), 1920, 1080), "ultrawide: pillarboxed 16:9");
  Check(size(UpscalerOutputSize(3440, 1440), 2560, 1440), "3440x1440: pillarboxed 16:9");
  Check(size(UpscalerOutputSize(1281, 721), 1281, 721), "odd window");
  Check(size(UpscalerOutputSize(1001, 777), 1001, 563), "odd window, taller than 16:9");
  Check(size(UpscalerOutputSize(800, 600), 800, 450), "4:3 window");
  {
    // The experimental Ultrawide mod (task 223, kk_aspect): the maths, then the sizes it gives the renderer.
    namespace uw = kk::ultrawide;
    auto near = [](double a, double b) { return std::abs(a - b) < 1e-6; };
    Check(near(uw::AspectFor("16:9", 2560, 1080), 16.0 / 9.0), "kk_aspect 16:9");
    Check(near(uw::AspectFor("", 2560, 1080), 16.0 / 9.0) && near(uw::AspectFor("4:3", 800, 600), 16.0 / 9.0),
          "kk_aspect unknown: 16:9");
    Check(near(uw::AspectFor("21:9", 1280, 720), 21.0 / 9.0) && near(uw::AspectFor("32:9", 0, 0), 32.0 / 9.0),
          "kk_aspect 21:9 and 32:9 whatever the window");
    Check(near(uw::AspectFor("auto", 2560, 1080), 2560.0 / 1080.0), "auto: the window's shape");
    Check(near(uw::AspectFor("auto", 1920, 1200), 16.0 / 9.0) && near(uw::AspectFor("auto", 7680, 1080), 32.0 / 9.0),
          "auto: 16:9 to 32:9");
    Check(uw::WidthFactor(16.0 / 9.0) == 1.0 && uw::WidthFactor(1.7779) == 1.0, "16:9: factor exactly 1");
    Check(near(uw::WidthFactor(21.0 / 9.0), 1.3125) && near(uw::WidthFactor(32.0 / 9.0), 2.0), "21:9 and 32:9 factors");
    Check(uw::LargestInside(2560, 1080, 21.0 / 9.0) == std::pair{2520, 1080}, "21:9 inside 2560x1080");
    Check(uw::LargestInside(1920, 1080, 21.0 / 9.0) == std::pair{1920, 823}, "21:9 inside 1080p: bars top and bottom");
    float m[16] = {1.5f, 0, 0, 0, 0, -2.6f, 0, 0, 0.25f, 0, 1.0f, 1.0f, 0, 0, -0.1f, 0};
    Check(uw::WidenProjection(m, 2.0f) && m[0] == 0.75f && m[8] == 0.125f && m[5] == -2.6f && m[10] == 1.0f,
          "perspective: x halved, y and depth as they were");
    float o[16] = {0.0015625f, 0, 0, 0, 0, -0.0027f, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};
    Check(!uw::WidenProjection(o, 2.0f) && o[0] == 0.0015625f && o[12] == -1.0f, "orthographic: unchanged");
    Check(!uw::WidenProjection(m, 1.0f) && m[0] == 0.75f, "factor 1: unchanged");
    // The sizes: 16:9 stays exactly as before; wider shapes fill the chosen resolution.
    const double a21 = 21.0 / 9.0, aw = 2560.0 / 1080.0;
    Check(size(UpscalerOutputSize(2560, 1080, 0, 0, 16.0 / 9.0), 1920, 1080), "16:9 given: as before");
    Check(size(UpscalerOutputSize(2560, 1080, 0, 0, aw), 2560, 1080), "auto at 2560x1080: the whole window");
    Check(size(UpscalerOutputSize(2560, 1080, 0, 0, a21), 2520, 1080), "21:9 at 2560x1080");
    Check(size(UpscalerOutputSize(3440, 1440, 0, 0, uw::AspectFor("auto", 3440, 1440)), 3440, 1440), "auto 3440x1440");
    Check(size(UpscalerOutputSize(5120, 1440, 0, 0, 32.0 / 9.0), 5120, 1440), "32:9 at 5120x1440");
    Check(size(NoUpscalerFrameSize(2560, 1080, 2560, 1440, aw), 2560, 1080), "no upscaler, native at 2560x1080");
    Check(size(NoUpscalerFrameSize(2560, 1080, 1280, 720, aw), 1707, 720), "no upscaler, 720p scale, ultrawide");
    Check(size(NoUpscalerFrameSize(2560, 1080, 2560, 1440), 1920, 1080), "no upscaler, 16:9: as before");
  }
  Check(size(UpscalerOutputSize(200, 100), 320, 180), "tiny window: the renderer's smallest scale");
  Check(size(UpscalerOutputSize(0, 0), 320, 180), "no size");
  Check(size(UpscalerOutputSize(1920, 1080, 1280, 720), 1280, 720), "custom 1x caps 1080p at 720p");
  Check(size(UpscalerOutputSize(1920, 1080, 3840, 2160), 1920, 1080), "custom 3x never above 1080p");
  for (int w = 640; w <= 7680; w += 37)
    for (int h = 360; h <= 4320; h += 53) {
      const auto [ow, oh] = UpscalerOutputSize(w, h);
      Check(ow <= w && oh <= h, "never above the chosen size");
      Check(ow == w || oh == h, "fills one side");
      Check(std::abs(ow * 9 - oh * 16) <= 16, "16:9");
    }
  // ApplyRenderPreset with an upscaler: native_upscaler_output is the chosen size (custom: capped).
  rex::cvar::SetFlagByName("native_upscaler", "off");
  ApplyRenderPreset(1920, 1080);
  Equals("native_upscaler_output", "");  // no upscaler: nothing asked
  Equals("native_frame_size", "1920x1080");  // custom 2x (2560x1440) without an upscaler: never above 1080p
  rex::cvar::SetFlagByName("native_upscaler", "dlss");
  ApplyRenderPreset(1920, 1080);
  Equals("native_upscaler_output", "1920x1080");  // custom resolution_scale 2 (2560x1440): the screen is less
  Equals("native_frame_size", "");  // with an upscaler its output is the frame
  rex::cvar::ResetAllToDefaults();
  rex::cvar::SetFlagByName("kk_render_quality", "quality");
  rex::cvar::SetFlagByName("native_upscaler", "dlss");
  ApplyRenderPreset(1920, 1080);
  Equals("native_upscaler_output", "1920x1080");
  Equals("draw_resolution_scale_x", "2");  // kept as before (logs; an upscaler turned off live)
  ApplyRenderPreset(2560, 1600);
  Equals("native_upscaler_output", "2560x1440");
  rex::cvar::SetFlagByName("native_upscaler", "off");
  ApplyRenderPreset(1920, 1080);
  Equals("native_upscaler_output", "");
  Equals("draw_resolution_scale_x", "1");  // no upscaler: unchanged (Quality at 1080p: 720p)
  Equals("native_frame_size", "1280x720");
  rex::cvar::SetFlagByName("native_upscaler", "fsr3");
  rex::cvar::SetFlagByName("kk_render_quality", "custom");
  rex::cvar::SetFlagByName("draw_resolution_scale_x", "1");
  rex::cvar::SetFlagByName("draw_resolution_scale_y", "1");
  ApplyRenderPreset(3840, 2160);
  Equals("native_upscaler_output", "1280x720");  // a Custom scale is the upper limit
  rex::cvar::SetFlagByName("draw_resolution_scale_x", "4");
  rex::cvar::SetFlagByName("draw_resolution_scale_y", "4");
  ApplyRenderPreset(2560, 1440);
  Equals("native_upscaler_output", "2560x1440");
  // Without an upscaler the frame is never above the chosen resolution either (DECISIONS 16 Oct, late):
  // the draw resolution scale's size, each axis at most the chosen resolution's 16:9 picture.
  struct NoUpscalerCase { int sw, sh, scale, fw, fh; const char* what; };
  for (const NoUpscalerCase& c : std::initializer_list<NoUpscalerCase>{
           {1920, 1080, 2, 1920, 1080, "1080p, Native (2x): 1920x1080, not 2560x1440 shrunk"},
           {1920, 1200, 2, 1920, 1080, "1200p, Native (2x): the 16:9 picture"},
           {2560, 1440, 2, 2560, 1440, "1440p, Native: unchanged"},
           {3840, 2160, 3, 3840, 2160, "4K, Native: unchanged"},
           {3840, 2160, 2, 2560, 1440, "4K, Quality (2x): below stays"},
           {1600, 900, 1, 1280, 720, "900p: 720p as before (below the chosen size is unchanged)"},
           {1366, 768, 1, 1280, 720, "1366x768: 720p as before"},
           {3200, 1800, 3, 3200, 1800, "1280x720 window at 250%: 3x capped to the window"},
           {2560, 1080, 2, 1920, 1080, "ultrawide 1080p: the 16:9 picture"},
           {1152, 648, 1, 1152, 648, "a window under 720p: never above it"},
           {1920, 1080, 3, 1920, 1080, "custom 3x at 1080p: 1080p"},
           {3840, 2160, 1, 1280, 720, "custom 1x (Original look) at 4K: 720p"},
           {200, 100, 1, 320, 180, "tiny window: the renderer's smallest"}}) {
    const auto [fw, fh] = NoUpscalerFrameSize(c.sw, c.sh, 1280 * c.scale, 720 * c.scale);
    Check(fw == c.fw && fh == c.fh, c.what);
  }
  for (int w = 640; w <= 7680; w += 41)
    for (int h = 360; h <= 4320; h += 59)
      for (int scale = 1; scale <= 8; ++scale) {
        const auto [fw, fh] = NoUpscalerFrameSize(w, h, 1280 * scale, 720 * scale);
        const auto [pw, ph] = UpscalerOutputSize(w, h);
        Check(fw <= pw && fh <= ph, "no upscaler: never above the chosen 16:9 picture");
        Check(fw <= 1280 * scale && fh <= 720 * scale, "no upscaler: never above the draw scale's size");
        Check(fw == std::min(1280 * scale, pw) && fh == std::min(720 * scale, ph), "no upscaler: the smaller");
      }
  rex::cvar::ResetAllToDefaults();
  rex::cvar::SetFlagByName("native_upscaler", "off");
  rex::cvar::SetFlagByName("kk_render_quality", "native");
  ApplyRenderPreset(1920, 1080);
  Equals("draw_resolution_scale_x", "2");  // the preset's 720p step, as before
  Equals("native_frame_size", "1920x1080");  // drawn at 1080p
  ApplyRenderPreset(1920, 1200);
  Equals("native_frame_size", "1920x1080");
  ApplyRenderPreset(2560, 1440);
  Equals("native_frame_size", "2560x1440");
  ApplyRenderPreset(3840, 2160);
  Equals("native_frame_size", "3840x2160");
  ApplyRenderPreset(1600, 900);
  Equals("native_frame_size", "1280x720");  // below the chosen size: unchanged
  rex::cvar::SetFlagByName("native_upscaler", "dlss");
  ApplyRenderPreset(1920, 1080);
  Equals("native_frame_size", "");
  Equals("native_upscaler_output", "1920x1080");
  rex::cvar::ResetAllToDefaults();
  rex::cvar::SetFlagByName("native_upscaler", "off");
  for (const auto& [name, value] : OriginalLookSettings()) rex::cvar::SetFlagByName(name, value);
  // A fresh start: the axes at the plugin's 1 (a preset above moved their defaults in this process).
  rex::cvar::SetFlagByName("draw_resolution_scale_x", "1");
  rex::cvar::SetFlagByName("draw_resolution_scale_y", "1");
  ApplyRenderPreset(1920, 1080);
  Equals("native_frame_size", "1280x720");  // the Original look: 720p
  ApplyRenderPreset(1152, 648);
  Equals("native_frame_size", "1152x648");  // ... never above a smaller window
  rex::cvar::ResetAllToDefaults();
  rex::cvar::SetFlagByName("kk_original_look", "true");
  const auto before = rex::cvar::GetFlagByName("native_ao_samples");
  ApplyHardwareRecommendation(true, 1920, 1080);
  Check(rex::cvar::GetFlagByName("native_ao_samples") == before, "Original look kept");
  rex::cvar::ResetAllToDefaults();
  std::ofstream(config) << "kk_frame_rate = 30\n";
  InitializeHardwareRecommendation(config, 1920, 1080);
  Equals("kk_hardware_tier", ""); // existing installations are untouched
  // Live graphics trials must survive an unrelated save without becoming persistent.
  rex::cvar::ResetAllToDefaults();
  rex::cvar::SetFlagByName("native_ao_samples", "2");
  SetTemporarySettingBaseline("native_ao_samples", "2");
  rex::cvar::SetFlagByName("native_ao_samples", "3");
  SetTemporarySettingTrialValue("native_ao_samples", "3");
  Check(SaveSettings(config), "save while graphics trial is active");
  std::ifstream trial_file(config);
  const std::string trial_text((std::istreambuf_iterator<char>(trial_file)), {});
  Check(trial_text.find("native_ao_samples = 2") != std::string::npos, "unrelated save keeps saved AO quality");
  Check(trial_text.find("native_ao_samples = 3") == std::string::npos, "trial AO quality never written");
  Check(!SaveSettings(dir / "missing-dir" / "settings.toml", true), "failed explicit save retains trial protection");
  Check(SaveSettings(config), "ordinary save still protects trial after failure");
  Check(SaveSettings(config, true), "explicitly save graphics trial");
  std::ifstream committed_file(config);
  const std::string committed_text((std::istreambuf_iterator<char>(committed_file)), {});
  Check(committed_text.find("native_ao_samples = 3") != std::string::npos, "explicit save writes trial AO quality");
  ClearTemporarySettingBaselines();
  // Task 200 (Kong mods): a settings mod's values are used while it is on and never saved; the player's
  // own values come back exactly (a value the player set, and one left at its default).
  {
    rex::cvar::ResetAllToDefaults();
    rex::cvar::SetFlagByName("kk_frame_rate", "120");  // the player's choice
    const std::string fog_default = rex::cvar::GetFlagByName("kk_fog");
    Check(SetModSetting("kk_frame_rate", "30"), "mod sets the frame rate");
    Check(SetModSetting("kk_fog", fog_default == "true" ? "false" : "true"), "mod sets fog");
    Equals("kk_frame_rate", "30");
    Check(SaveSettings(config), "save while a settings mod is on");
    std::ifstream f(config);
    const std::string text((std::istreambuf_iterator<char>(f)), {});
    Check(text.find("kk_frame_rate = 120") != std::string::npos, "mod: the player's frame rate is saved");
    Check(text.find("kk_frame_rate = 30") == std::string::npos, "mod: the mod's frame rate is never saved");
    Check(text.find("kk_fog") == std::string::npos, "mod: a default left alone stays out of the file");
    // The player changes the setting while the mod is on: their new choice is saved.
    rex::cvar::SetFlagByName("kk_frame_rate", "90");
    Check(SaveSettings(config), "save after the player's change");
    std::ifstream g(config);
    const std::string text2((std::istreambuf_iterator<char>(g)), {});
    Check(text2.find("kk_frame_rate = 90") != std::string::npos, "mod: the player's later change is saved");
    // Mod off (the next start reads the file): exactly the player's settings.
    ClearModSettings();
    rex::cvar::ResetAllToDefaults();
    rex::cvar::SetFlagByName("kk_frame_rate", "120");
    Check(SetModSetting("kk_frame_rate", "30") && SetModSetting("kk_frame_rate", "45"), "two mods, same setting");
    Check(SaveSettings(config), "save with two mods");
    std::ifstream h(config);
    const std::string text3((std::istreambuf_iterator<char>(h)), {});
    Check(text3.find("kk_frame_rate = 120") != std::string::npos, "mod: two mods still save the player's value");
    ClearModSettings();
    Check(!SetModSetting("no_such_setting_x", "1"), "mod: unknown setting refused");
    ClearModSettings();
  }
  // Version 3 recognises every old preset without touching its other settings.
  for (const auto& p : kGraphicsPresets) {
    rex::cvar::ResetAllToDefaults();
    ApplyPortDefaults();
    rex::cvar::SetFlagByName("kk_settings_version", "2");
    ApplyGraphicsPresetSettings(p, [](const char* n, const char* v) { rex::cvar::SetFlagByName(n, v); });
    rex::cvar::ResetToDefault("native_gi"); // old files omitted the default Off
    Check(SaveSettings(config), "write old preset");
    rex::cvar::ResetAllToDefaults();
    rex::cvar::LoadConfig(config);
    MigrateSettings(config);
    Equals("native_gi", p.gi);
    Equals("kk_settings_version", "3");
    Equals("kk_render_quality", p.quality);
    Check(!std::filesystem::exists(config.string() + ".bak"), "valid version 2 is not mistaken for unread settings");
    MigrateSettings(config);
    Equals("native_gi", p.gi);
  }
  for (const char* gi : {"off", "low", "medium", "high"}) {
    rex::cvar::ResetAllToDefaults();
    ApplyPortDefaults();
    rex::cvar::SetFlagByName("kk_settings_version", "2");
    rex::cvar::SetFlagByName("native_gi", gi);
    Check(SaveSettings(config), "save explicit GI including the current default");
    rex::cvar::ResetAllToDefaults();
    rex::cvar::LoadConfig(config);
    MigrateSettings(config);
    ApplyHardwareRecommendation(true, 1920, 1080);
    Equals("native_gi", gi);
  }
  // Non-file overrides win over migration too.
  for (auto source : {rex::cvar::Source::kEnvironment, rex::cvar::Source::kCommandLine}) {
    rex::cvar::ResetAllToDefaults();
    std::ofstream(config) << "kk_settings_version = 2\n";
    rex::cvar::LoadConfig(config);
    for (auto& e : rex::cvar::GetRegistry()) if (e.name == "native_gi") { e.setter("off"); e.source = source; }
    MigrateSettings(config);
    Equals("native_gi", "off");
  }
  rex::cvar::ResetAllToDefaults();
  ApplyPortDefaults();
  std::ofstream(config) << "kk_settings_version = 2\nkk_original_look = true\n"
      "kk_modern_settings = \"kk_render_quality=native;swap_post_effect=fxaa;"
      "anisotropic_override=5;ao_mode=1;ao_strength=3;\"\n";
  rex::cvar::LoadConfig(config);
  MigrateSettings(config);
  EnforceOriginalLook();
  Equals("native_gi", "off");
  Check(REXCVAR_GET(kk_modern_settings).find("native_gi=high;") != std::string::npos,
        "Original keeps a migrated Ultra GI in its saved Modern settings");
  rex::cvar::ResetAllToDefaults();
  for (const char* gi : {"off", "low", "medium", "high"}) {
    rex::cvar::ResetAllToDefaults();
    std::ofstream(config) << "kk_settings_version = 2\nkk_original_look = true\n"
        "kk_modern_settings = \"native_gi=" << gi << ";kk_render_quality=native;\"\n";
    rex::cvar::LoadConfig(config);
    MigrateSettings(config);
    Check(REXCVAR_GET(kk_modern_settings).find(std::string("native_gi=") + gi + ";") != std::string::npos,
          "Original preserves every saved Modern GI choice, including Off");
  }
  rex::cvar::ResetAllToDefaults();
  std::ofstream(config) << "kk_frame_rate = 144\n";
  rex::cvar::LoadConfig(config);
  MigrateSettings(config);
  Equals("present_effect", "bilinear");
  Equals("kk_render_quality", "native");
  Equals("kk_frame_rate", "144");
  Equals("kk_settings_version", "3");
  // A failed config load still must not overwrite the player's settings.
  rex::cvar::ResetAllToDefaults();
  const std::string unread = "kk_settings_version = 2\ninvalid settings\n";
  std::ofstream(config) << unread;
  MigrateSettings(config);
  std::ifstream untouched(config);
  Check(std::string((std::istreambuf_iterator<char>(untouched)), {}) == unread, "unread settings left intact");
  Check(std::filesystem::exists(config.string() + ".bak"), "unread settings backed up");
  rex::cvar::ResetAllToDefaults();
  ApplyPortDefaults();
  MigrateSettings(dir / "new-install.toml");
  Equals("kk_settings_version", "3");
  InitializeHardwareRecommendation(dir / "new-install.toml", 1920, 1080);
  Equals("native_gi", "high");
  Check(!std::filesystem::exists(dir / "new-install.toml"), "migration does not create a new-install config early");
  rex::cvar::ResetAllToDefaults();
  Check(rex::cvar::SetFlagFromCommandLine("native_ao_samples", "1"), "set command-line AO quality");
  SetTemporarySettingBaseline("native_ao_samples", "1");
  rex::cvar::SetFlagByName("native_ao_samples", "3");
  SetTemporarySettingTrialValue("native_ao_samples", "3");
  Check(SaveSettings(config), "save during command-line AO trial");
  std::ifstream override_file(config);
  const std::string override_text((std::istreambuf_iterator<char>(override_file)), {});
  Check(override_text.find("native_ao_samples =") == std::string::npos, "command-line baseline remains excluded");
  ClearTemporarySettingBaselines();
  rex::cvar::SetFlagByName("native_ao_samples", "1");
  SetTemporarySettingBaseline("native_ao_samples", "1");
  rex::cvar::SetFlagByName("native_ao_samples", "2");
  SetTemporarySettingBaseline("native_ao_samples", "2"); // new saved choice after resetting a previous trial
  rex::cvar::SetFlagByName("native_ao_samples", "3");
  SetTemporarySettingTrialValue("native_ao_samples", "3");
  Check(SaveSettings(config), "save after recapturing a changed baseline");
  std::ifstream recaptured_file(config);
  const std::string recaptured_text((std::istreambuf_iterator<char>(recaptured_file)), {});
  Check(recaptured_text.find("native_ao_samples = 2") != std::string::npos, "new saved baseline replaces an older trial baseline");
  ClearTemporarySettingBaselines();
  // Ultrawide mod: ApplyRenderPreset: 16:9 leaves the presenter's shape alone; auto at 2560x1080 sets it and the factor.
  rex::cvar::ResetAllToDefaults();
  ApplyRenderPreset(2560, 1080);
  Equals("native_display_aspect", "");
  Check(UltrawideFactor() == 1.0f, "16:9: factor 1");
  rex::cvar::SetFlagByName("kk_aspect", "auto");
  ApplyRenderPreset(2560, 1080);
  Equals("native_display_aspect", "2560x1080");
  Check(std::abs(UltrawideFactor() - float(2560.0 / 1080.0 / (16.0 / 9.0))) < 1e-5f, "auto at 2560x1080: factor 1.333");
  rex::cvar::ResetAllToDefaults();
  std::cout << "PASS: " << checks << " hardware/settings checks; evidence " << dir.string() << '\n';
}
