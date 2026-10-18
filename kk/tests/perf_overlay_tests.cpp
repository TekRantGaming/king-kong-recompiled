// Task 206 (owner, 18 Oct): brightness and gamma (native-renderer/backend/picture_adjust.h), the Performance
// metrics overlay's logic (perf_overlay_logic.h) and the whole-GPU load it shows (benchmark_other_gpu.h).
// No window, game or GPU needed.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "backend/picture_adjust.h"
#include "benchmark_other_gpu.h"
#include "perf_overlay_logic.h"

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

std::array<uint16_t, 768> IdentityRamp() {
  std::array<uint16_t, 768> r;
  for (uint32_t c = 0; c < 3; ++c)
    for (uint32_t i = 0; i < 256; ++i) r[c * 256 + i] = uint16_t(i * 257);
  return r;
}

void PictureTests() {
  using nr::PictureAdjust;
  // The defaults are the game's own picture: identity, from the settings' text too.
  Check(PictureAdjust{}.identity(), "default is identity");
  Check(PictureAdjust::FromSettings(0, 1.0).identity(), "settings 0 / 1.0 are identity");
  Check(PictureAdjust::FromSettings(0, 1.000000119).identity(), "a slider's float text for 1 is identity");
  Check(PictureAdjust::FromSettings(NAN, NAN).identity(), "unreadable settings are the defaults");
  Check(!PictureAdjust::FromSettings(1, 1.0).identity() && !PictureAdjust::FromSettings(0, 1.05).identity(),
        "one step of either slider is not identity");
  // Clamped to the sliders' ranges.
  Check(PictureAdjust::FromSettings(80, 3.0).brightness == 1.0f && PictureAdjust::FromSettings(80, 3.0).gamma == 1.5f,
        "brightness and gamma are clamped");
  // Identity curve and ramp change nothing.
  const PictureAdjust id;
  for (int i = 0; i <= 255; ++i) Check(id.Apply(i / 255.0f) == i / 255.0f, "identity curve");
  Check(id.ComposeRamp(IdentityRamp(), true) == IdentityRamp(), "identity over an identity ramp is the identity ramp");
  // White stays white (HDR paper white stays where it is), black stays black with gamma alone.
  for (double b : {-50.0, -20.0, 0.0, 20.0, 50.0})
    for (double g : {0.5, 0.8, 1.0, 1.25, 1.5}) {
      const PictureAdjust p = PictureAdjust::FromSettings(b, g);
      Check(std::fabs(p.Apply(1.0f) - 1.0f) < 1e-6f, "white stays white");
      float last = -1;
      bool monotonic = true;
      for (int i = 0; i <= 255; ++i) {
        const float v = p.Apply(i / 255.0f);
        monotonic = monotonic && v >= last && v >= 0 && v <= 1;
        last = v;
      }
      Check(monotonic, "the curve rises and stays in 0..1");
      if (b == 0) Check(p.Apply(0.0f) == 0.0f, "gamma keeps black");
    }
  // Brightness raises (or lowers) the dark tones; gamma above 1 brightens the middle tones.
  Check(PictureAdjust::FromSettings(50, 1).Apply(0.0f) > 0.15f, "brightness +50 lifts black to grey");
  Check(PictureAdjust::FromSettings(-50, 1).Apply(0.15f) == 0.0f, "brightness -50 takes the darkest tones to black");
  Check(PictureAdjust::FromSettings(0, 1.5).Apply(0.5f) > 0.6f, "gamma 1.5 brightens the middle");
  Check(PictureAdjust::FromSettings(0, 0.5).Apply(0.5f) < 0.3f, "gamma 0.5 darkens the middle");
  // The Present pass at the defaults: exactly the old choice (ramp only when the game changed it, as it set it).
  std::array<uint16_t, 768> zero{};
  std::array<uint16_t, 768> custom = IdentityRamp();
  custom[10] = 1234;
  Check(!PictureAdjust::PresentUsesRamp(IdentityRamp(), id) && !PictureAdjust::PresentUsesRamp(zero, id) &&
            PictureAdjust::PresentUsesRamp(custom, id) && id.PresentRamp(custom) == custom,
        "defaults: no ramp for an identity or unset game ramp, the game's own ramp untouched otherwise");
  const PictureAdjust one = PictureAdjust::FromSettings(1, 1.0);
  Check(PictureAdjust::PresentUsesRamp(IdentityRamp(), one) && PictureAdjust::PresentUsesRamp(zero, one) &&
            one.PresentRamp(zero) == one.ComposeRamp(zero, true),
        "a player setting uses the ramp, over the identity when the game never set one");
  // The curve goes after the game's own ramp.
  std::array<uint16_t, 768> game = IdentityRamp();
  for (auto& v : game) v = uint16_t(v / 2);  // the game halves everything
  const PictureAdjust lift = PictureAdjust::FromSettings(0, 1.5);
  const auto composed = lift.ComposeRamp(game, false);
  Check(composed[128] == uint16_t(std::lround(lift.Apply(game[128] / 65535.0f) * 65535.0f)),
        "the player's curve is applied to the game's ramp value");
  Check(composed[255] < 65535, "the game's ramp still limits white");
  // The pause menu's kept frame: no change when nothing changed, and from one setting to another.
  // (Only the values the kept frame can hold: those the settings it was drawn with give.)
  const auto same = PictureAdjust::RetouchTable(lift, lift);
  bool exact = true;
  for (int i = 0; i < 256; ++i) {
    const int v = int(std::lround(lift.Apply(i / 255.0f) * 255));
    exact = exact && std::abs(int(same[v]) - v) <= 1;
  }
  Check(exact, "retouching with the same settings keeps the picture (to one step)");
  const auto from_default = PictureAdjust::RetouchTable(id, lift);
  bool matches = true;
  for (int v = 0; v < 256; ++v) matches = matches && from_default[v] == uint8_t(std::lround(lift.Apply(v / 255.0f) * 255));
  Check(matches, "retouching a default frame gives the new settings' picture");
  const auto back = PictureAdjust::RetouchTable(lift, id);
  bool inverse = true;
  for (int v = 0; v < 256; ++v)
    inverse = inverse && std::abs(int(back[from_default[v]]) - v) <= 3;
  Check(inverse, "retouching back to the defaults comes close to the original picture");
}

void OverlayTests() {
  using namespace kk::perf;
  Check(ParseLevel("off") == Level::kOff && ParseLevel("fps") == Level::kFps && ParseLevel("full") == Level::kFull &&
            ParseLevel("") == Level::kOff && ParseLevel("bogus") == Level::kOff,
        "levels parse; anything else is off");
  Check(NextLevel(Level::kOff) == Level::kFps && NextLevel(Level::kFps) == Level::kFull &&
            NextLevel(Level::kFull) == Level::kOff,
        "F2 goes Off, FPS only, Full, Off");
  for (Level l : {Level::kOff, Level::kFps, Level::kFull}) Check(ParseLevel(LevelValue(l)) == l, "level values round trip");
  Check(GraphTop(16.7f) == 20 && GraphTop(33.4f) == 35 && GraphTop(48) == 50 && GraphTop(80) == 100 &&
            GraphTop(150) == 200 && GraphTop(900) == 200,
        "graph tops: 60 FPS and 30 FPS sit low, hitches reach the top");
  Check(GiB(1638.4) == "1.6" && GiB(-1) == "-" && Whole(45.4) == "45" && Whole(-1) == "-" && Whole(NAN) == "-",
        "number formats");
  Check(UpscalerShort("DLSS 310.9.1 Quality preset M (Vulkan)") == "DLSS 310.9.1 Quality preset M" &&
            UpscalerShort("off") == "off",
        "the upscaler line drops the API in brackets");
  Check(FpsBand(60) == 0 && FpsBand(30) == 1 && FpsBand(20) == 2, "frame rate colours");
}

void GpuLoadTests() {
  using kk::benchmark::AdapterGpuLoad;
  using kk::benchmark::GpuInstance;
  const std::wstring g3d = L"pid_100_luid_0x0_0x1_phys_0_eng_0_engtype_3D";
  const std::wstring gcopy = L"pid_100_luid_0x0_0x1_phys_0_eng_1_engtype_Copy";
  const std::wstring o3d = L"pid_200_luid_0x0_0x1_phys_0_eng_0_engtype_3D";
  const std::wstring other_adapter = L"pid_200_luid_0x0_0x2_phys_0_eng_0_engtype_3D";
  std::vector<GpuInstance> in = {{g3d, 50}, {gcopy, 10}, {o3d, 30}, {other_adapter, 90}};
  Check(std::fabs(AdapterGpuLoad(in, 100) - 80) < 1e-9, "whole card: every process on the game's adapter, busiest engine");
  in.push_back({o3d, 40});
  Check(AdapterGpuLoad(in, 100) == 100, "capped at 100");
  Check(AdapterGpuLoad({{o3d, 30}}, 100) == -1, "unknown without the game's adapter");
  Check(AdapterGpuLoad({{g3d, 0}, {o3d, 12}}, 100) == 12, "idle game: its adapter still counts other programs");
}
}  // namespace

int main() {
  PictureTests();
  OverlayTests();
  GpuLoadTests();
  if (failures) return 1;
  std::printf("picture and performance overlay tests passed\n");
  return 0;
}
