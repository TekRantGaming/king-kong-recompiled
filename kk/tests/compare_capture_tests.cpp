// Task 354: the comparison capture's logic (compare_capture_logic.h).
#include "compare_capture_logic.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace kk::compare;

namespace {
void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", what);
    std::exit(1);
  }
}
std::string Value(const std::vector<Setting>& list, const char* name) {
  for (const auto& [n, v] : list)
    if (std::string(n) == name) return v;
  return "<none>";
}
}  // namespace

int main() {
  // The shot list: comments, blank lines, a BOM, missing parts, unsafe ids.
  const auto shots = ParseShots(
      "\xEF\xBB\xBF# comment\n\nlook-1 | Overall look | A wide view\r\n"
      "gi|Global illumination|Bounce light\n"
      "water reflections | Water\n"
      "detail\n"
      "   | no id | skipped\n");
  Check(shots.size() == 4, "four shots");
  Check(shots[0].id == "look-1" && shots[0].feature == "Overall look" && shots[0].hint == "A wide view", "first shot");
  Check(shots[1].id == "gi" && shots[1].hint == "Bounce light", "no spaces around bars");
  Check(shots[2].id == "water-reflections" && shots[2].feature == "Water" && shots[2].hint.empty(), "unsafe id, no hint");
  Check(shots[3].id == "detail" && shots[3].feature == "detail", "id only");
  Check(DefaultShots().size() == 10 && DefaultShots()[0].id == "look-1" && DefaultShots()[9].id == "detail",
        "the owner's start list, in order");
  Check(ParseShots("# only comments\n").empty(), "no shots");

  // 720 lines from the output height.
  Check(std::string(OriginalModeFor(2160).quality) == "ultra_performance", "4K: 3x");
  Check(std::string(OriginalModeFor(1440).quality) == "performance", "1440p: 2x");
  Check(std::string(OriginalModeFor(1080).quality) == "quality", "1080p: 1.5x");
  Check(std::string(OriginalModeFor(720).quality) == "native", "720p: 1x");
  Check(ParseSize("3840x2160") == std::pair{3840, 2160} && ParseSize("auto") == std::pair{0, 0}, "sizes");

  // The Max look's upscaler.
  Check(BestUpscaler("fsr3,dlss,xess") == "dlss", "DLSS first");
  Check(BestUpscaler("xess, fsr3") == "xess", "XeSS before FSR 3.1");
  Check(BestUpscaler("none") == "fsr3" && BestUpscaler("") == "fsr3", "FSR 3.1 when nothing is listed");

  // Both looks set the same settings (a switch leaves nothing of the other behind), the same field of view, and
  // only live settings.
  const auto o = OriginalSettings(2160);
  const auto m = MaxSettings("dlss", "C:/mods/Kong UHD Textures/textures");
  for (const auto& [name, value] : o) {
    (void)value;
    const std::string n = name;
    Check(n != "kk_original_look" && n != "kk_render_quality" && n != "present_effect" && n != "native_backend" &&
              n != "resolution_scale",
          "no restart-only setting in a look");
    if (n != "native_upscaler_dlss_preset" && n != "native_ao_quality" && n != "native_ao_samples" &&
        n != "native_ao_temporal" && n != "native_gi_distance" && n != "native_gi_quality")
      Check(Value(m, name) != "<none>", "Max sets every Original setting");
  }
  Check(Value(o, "kk_fov") == Value(m, "kk_fov"), "the same field of view");
  Check(Value(o, "native_upscaler") == "bilinear" && Value(o, "native_upscaler_still") == "true" &&
            Value(o, "native_upscaler_mip_bias") == "0",
        "Original: a still bilinear stretch at the game's texture bias");
  Check(Value(m, "native_upscaler_quality") == "native" && Value(m, "native_upscaler_still") == "false", "Max: native AA");
  Check(Value(o, "native_texture_pack") == "false" && Value(m, "native_texture_pack") == "true", "UHD pack in Max only");
  Check(Value(OriginalSettings(2160, "x"), "native_texture_pack_folder") == "x" &&
            Value(m, "native_texture_pack_folder") == "C:/mods/Kong UHD Textures/textures",
        "the same pack folder in both looks");
  Check(Value(MaxSettings("dlss", ""), "native_texture_pack") == "false" &&
            Value(MaxSettings("dlss", ""), "native_texture_pack_folder") == "<none>",
        "no UHD pack: the game's textures, the player's folder left alone");

  // The settle measure.
  std::vector<uint8_t> a(4 * 4 * 4, 100), b = a;
  Check(MeanDifference(a.data(), b.data(), 4, 4, 16, 16, 1) == 0.0, "identical");
  for (size_t i = 0; i < b.size(); i += 4) b[i] = 106;  // red +6 everywhere
  Check(std::abs(MeanDifference(a.data(), b.data(), 4, 4, 16, 16, 1) - 2.0) < 1e-9, "mean of the channels");
  // A few pixels changing a lot (rain) leave the 90th percentile alone; most pixels changing a little do not.
  std::vector<uint8_t> c(10 * 10 * 4, 50), r = c, g = c;
  for (int i = 0; i < 5; ++i) r[size_t(i) * 4] = 250;
  for (size_t i = 0; i < g.size(); i += 4) g[i + 1] = 53;
  Check(Difference(c.data(), r.data(), 10, 10, 40, 40, 1).p90 == 0, "rain: settled");
  Check(Difference(c.data(), g.data(), 10, 10, 40, 40, 1).p90 == 3, "converging: not settled");
  std::puts("compare capture: all checks passed");
  return 0;
}
