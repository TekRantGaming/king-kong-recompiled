// The high frame rate fix "fish" (src/hfr_fish.cpp): the swimming creatures'
// heading recurrence of PNJ_EatMe_ETAT_Water, modelled as the game runs it, must
// swing the same amount per second of game time at any frame rate.
#include "hfr_fish_logic.h"

#include <algorithm>
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

struct Swing {
  double amplitude;  // half the heading's range, radians
  double rate;       // mean |heading change| per game second
};

// The game's per-call steps (sub_8243B2C8): turn 6 x dt of the way to the course
// (sub_82719648, clamped to 1), advance the swim phase by pi x dt and wrap it at
// 2 pi, then turn by sin(phase) x the capped forward speed (0.25 for a fish that
// swims). Heading measured from the course, in the plane.
Swing Run(double fps, bool fixed) {
  const float dt = float(1.0 / fps);
  float heading = 0, phase = 0;
  double low = 1e9, high = -1e9, travelled = 0, t = 0;
  while (t < 40) {
    heading += std::min(6.0f * dt, 1.0f) * (0.0f - heading);
    phase += 3.14159274f * dt;
    if (phase > 6.28318548f) phase -= 6.28318548f;
    float angle = std::sin(phase) * 0.25f;
    if (fixed && FishWiggleApplies(dt)) angle = FishWiggle(angle, dt);
    const float before = heading;
    heading += angle;
    t += dt;
    if (t > 10) {
      low = std::min(low, double(heading));
      high = std::max(high, double(heading));
      travelled += std::fabs(double(heading - before));
    }
  }
  return {(high - low) / 2, travelled / (t - 10)};
}

}  // namespace

int main() {
  Check(FishWiggleApplies(1.0f / 30) && FishWiggleApplies(0.001f) && FishWiggleApplies(0.063f),
        "the game's frame time range is corrected");
  Check(!FishWiggleApplies(0.0f) && !FishWiggleApplies(0.0005f) && !FishWiggleApplies(0.07f) &&
            !FishWiggleApplies(std::nanf("")),
        "a frame time the game cannot produce keeps the game's angle");
  Check(std::fabs(FishWiggle(0.25f, 1.0f / 30) - 0.25f) < 1e-6f, "a console frame keeps the game's angle");
  Check(std::fabs(FishWiggle(-0.2f, 1.0f / 120) + 0.05f) < 1e-6f, "a 120 FPS frame turns a quarter as far");

  const Swing console = Run(30, false);
  std::printf("30 FPS (game): swing %.3f rad, %.3f rad/s\n", console.amplitude, console.rate);
  Check(console.amplitude > 1.0 && console.amplitude < 1.3, "the console's swing is about +-65 degrees");
  for (double fps : {60.0, 90.0, 120.0, 144.0, 165.0, 240.0, 500.0}) {
    const Swing game = Run(fps, false), fix = Run(fps, true);
    std::printf("%g FPS: game %.3f rad %.3f rad/s, fixed %.3f rad %.3f rad/s\n", fps, game.amplitude, game.rate,
                fix.amplitude, fix.rate);
    Check(std::fabs(fix.amplitude / console.amplitude - 1) < 0.03, "fixed swing matches the console's");
    Check(std::fabs(fix.rate / console.rate - 1) < 0.03, "fixed turn rate matches the console's");
    Check(game.amplitude / console.amplitude > fps / 30 * 0.9, "unfixed swing grows with the frame rate");
  }
  std::printf("ok\n");
  return 0;
}
