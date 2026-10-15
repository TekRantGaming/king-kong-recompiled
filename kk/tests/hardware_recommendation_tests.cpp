#include "hardware_recommendation.h"
#include "settings.h"
#include "graphics_presets.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>

// This console test links the real settings implementation and cvar registry,
// with OS detection replaced by controlled profiles. No game/device is started.
REXCVAR_DEFINE_STRING(native_upscaler, "off", "Test", "Test");
REXCVAR_DEFINE_STRING(native_backend, "auto", "Test", "Test");
REXCVAR_DEFINE_STRING(gpu_plugin, "native", "Test", "Test");
REXCVAR_DEFINE_STRING(native_upscaler_quality, "quality", "Test", "Test");
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
  Check(rex::cvar::GetFlagByName(name) == expected, name);
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
  ApplyRenderPreset(1080);
  Equals("resolution_scale", "2");
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
  std::cout << "PASS: " << checks << " hardware/settings checks; evidence " << dir.string() << '\n';
}
