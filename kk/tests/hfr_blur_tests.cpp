// The high frame rate fix "blur" (src/motion_blur.cpp, src/hfr_blur_logic.h):
// the game's feedback motion blur, modelled as it runs (each frame the kept
// image is blended over the new one with the alpha, then kept), must fade an
// image by the same amount per second at any frame rate as at 30 FPS.
#include "hfr_blur_logic.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace kk::hfr;

namespace {

void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", what);
    std::exit(1);
  }
}

// The alpha the shader gets at `fps` (the game's fps estimate = the frame rate).
float Alpha(float s, double fps, bool fixed) {
  const float f = float(fps);
  if (!fixed || fps <= 30.0) return GameBlurAlpha(s, f);
  return GameBlurAlpha(StrengthForAlpha(BlurAlpha30(s, float(1.0 / fps)), f), f);
}

// What is left of a one-frame image after `seconds` at `fps` (the feedback blend).
double Left(float s, double fps, double seconds, bool fixed) {
  const double a = Alpha(s, fps, fixed);
  return std::pow(a, std::lround(seconds * fps));
}

}  // namespace

int main() {
  for (float s : {0.1f, 0.3f, 0.44f, 0.6f, 0.9f}) {
    Check(GameBlurAlpha(s, 30.0f) == s, "the game's alpha at 30 FPS is the strength");
    const double at30 = Left(s, 30, 0.2, false);
    for (double fps : {60.0, 120.0, 144.0, 240.0, 500.0}) {
      const double fixed = Left(s, fps, 0.2, true);
      Check(std::fabs(std::log(fixed) - std::log(at30)) < 0.01 * std::fabs(std::log(at30)) + 1e-6,
            "the trail fades per second as at 30 FPS");
    }
    // Unfixed, the game's linear fps term gives a different trail at 120 FPS.
    Check(std::fabs(std::log(Left(s, 120, 0.2, false) + 1e-30) - std::log(at30)) > 0.1 * std::fabs(std::log(at30)),
          "unfixed 120 FPS trail differs");
  }
  Check(BlurAlpha30(0.0f, 0.008f) == 0.0f, "no blur stays no blur");
  Check(BlurAlpha30(1.0f, 0.008f) == 1.0f, "a full hold at 30 stays a full hold");
  Check(GameBlurAlpha(0.5f, 120.0f) == 1.0f, "unfixed: s 0.5 at 120 FPS freezes the trail");
  Check(Alpha(0.5f, 120.0, true) < 0.85f, "fixed: s 0.5 at 120 FPS fades");
  std::puts("hfr blur: ok");
  return 0;
}
