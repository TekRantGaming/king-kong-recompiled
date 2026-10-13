// The temporal upscalers' settings (docs/upscalers.md): which upscaler, which quality mode, and the
// knobs behind them. No GPU, no SDK: parsed from the plugin's cvars and passed to the game renderer.
//
// --native_upscaler          off (default), auto, fsr3, fsr4, dlss, xess, and bilinear (a plain
//                            bilinear stretch with the same frame plumbing: tests and comparisons)
// --native_upscaler_quality  native (native-resolution anti-aliasing: DLAA, FSR native AA, XeSS native
//                            AA), quality, balanced, performance, ultra_performance
// The output resolution is the render scale's (--native_render_scale, auto: the display's); the input
// is the output divided by the mode's ratio (each SDK's own ratio where it has one).
#pragma once

#include <cstdint>
#include <string>

namespace nr {

enum class UpscalerKind : uint8_t {
  kOff,
  kAuto,      // resolved at start: DLSS on NVIDIA RTX, FSR 4 on supported AMD, XeSS on Intel, FSR 3.1 otherwise
  kFsr3,      // AMD FidelityFX Super Resolution 3.1 (every GPU; Vulkan and D3D12)
  kFsr4,      // AMD FSR 4 (machine learning; D3D12 on supported AMD GPUs; else FSR 3.1)
  kDlss,      // NVIDIA DLSS Super Resolution / DLAA (RTX GPUs)
  kXess,      // Intel XeSS Super Resolution (every GPU with SM 6.4 and DP4a; best on Intel Arc)
  kBilinear,  // a plain bilinear stretch, no temporal accumulation (tests, comparisons)
};
const char* UpscalerKindName(UpscalerKind k);
bool ParseUpscalerKind(const std::string& text, UpscalerKind& out);

enum class UpscaleQuality : uint8_t { kNative, kQuality, kBalanced, kPerformance, kUltraPerformance };
const char* UpscaleQualityName(UpscaleQuality q);
bool ParseUpscaleQuality(const std::string& text, UpscaleQuality& out);
// The per-axis ratio (output / input) FSR and DLSS document for a mode: 1, 1.5, 1.7, 2, 3. XeSS 1.3 and
// later use other ratios for the same names; the XeSS upscaler asks its SDK.
float DefaultUpscaleRatio(UpscaleQuality q);

// Where in the frame the upscaler runs (docs/upscalers.md, "Where it runs").
enum class UpscalePoint : uint8_t {
  kPost,     // before the game's post effects and HUD (the default): the scene at the input size, the
             // post chain and the HUD at the output size
  kPresent,  // at Present: everything (post effects and HUD too) at the input size, upscaled at the end
};

struct UpscaleConfig {
  UpscalerKind kind = UpscalerKind::kOff;
  UpscaleQuality quality = UpscaleQuality::kQuality;
  UpscalePoint point = UpscalePoint::kPost;
  // DLSS's render preset: 0 the driver's default, kDlssPresetLatest the newest model the DLL has
  // (DLSS 4.5: M, and L for ultra performance, as NVIDIA recommends), else the letter (J and K: DLSS 4's
  // transformer model; L and M: DLSS 4.5's second-generation transformer). Other letters are refused by
  // NGX and fall back to its default.
  char dlss_preset = 0;
  // Sharpening after the upscale, 0 to 1 (FSR's RCAS; DLSS 3.1+ and XeSS have none). Negative: off.
  float sharpness = -1.0f;
  // The texture LOD bias of the scene's draws: with mip_bias_auto (the default) log2(input / output)
  // as the SDKs recommend; else mip_bias (0: none).
  float mip_bias = 0.0f;
  bool mip_bias_auto = true;
  // The reactive (and DLSS bias-current-colour, XeSS responsive) mask from the draws after the opaque
  // pass: its strength, 0 to 1 (0: no mask).
  float reactive = 1.0f;
  // Debugging (bits): 1 the output shows the motion vectors, 2 the reactive mask, 4 camera motion only
  // (no per-object vectors), 8 no jitter, 16 a log line per upscaled frame, 32 the depth the upscaler
  // gets, 64 no mip bias.
  uint32_t debug = 0;
  // GPU timer queries around the work at the upscale point and around the upscaler's own work,
  // logged every 600 frames (--native_upscaler_gpu_time).
  bool gpu_time = false;
  // A rule file (UpscaleRules: "post ps=<hash>", "scene ps=<hash>"), read at start.
  std::string rules_file;

  bool enabled() const { return kind != UpscalerKind::kOff; }
};

// native_upscaler_dlss_preset "latest" (the default): the newest model, chosen per mode (DlssPresetFor).
constexpr char kDlssPresetLatest = '*';
// The preset letter for a mode (0: the DLL's default): kDlssPresetLatest gives M, or L for ultra performance.
char DlssPresetFor(char preset, UpscaleQuality quality);

// "--native_upscaler_dlss_preset": "default" (or empty), "latest" or one letter. False when not understood.
bool ParseDlssPreset(const std::string& text, char& out);

// The input size for an output size and a ratio, as the SDKs round it (to the nearest pixel, at
// least 1).
uint32_t UpscaleInputSize(uint32_t output, float ratio);

// One log line describing the configuration.
std::string DescribeUpscaleConfig(const UpscaleConfig& c);

}  // namespace nr
