// Task 330: the live main menu scene's rules (menu_scene_logic.h).
#include <cstdio>
#include <cstdlib>

#include "menu_scene_logic.h"

using namespace kk::menu_scene;

static int failures = 0;
#define CHECK(x)                                                   \
  do {                                                             \
    if (!(x)) {                                                    \
      std::printf("FAILED line %d: %s\n", __LINE__, #x);           \
      ++failures;                                                  \
    }                                                              \
  } while (0)

int main() {
  // The decision (no setting, owner 20 Oct): live unless the scene cannot or should not run; the first reason wins.
  StartFacts f;
  CHECK(Decide(f) == Reason::kLive);
  { StartFacts g = f; g.files_ok = false; CHECK(Decide(g) == Reason::kNoFiles); }
  { StartFacts g = f; g.native_renderer = false; CHECK(Decide(g) == Reason::kNotNative); }
  { StartFacts g = f; g.shaders_prepared = false; CHECK(Decide(g) == Reason::kShadersNotPrepared); }
  { StartFacts g = f; g.shader_job = true; CHECK(Decide(g) == Reason::kShaderJob); }
  { StartFacts g = f; g.setup = true; CHECK(Decide(g) == Reason::kSetup); }
  { StartFacts g = f; g.low_end = true; CHECK(Decide(g) == Reason::kLowEnd); }
  { StartFacts g = f; g.off_this_session = true; CHECK(Decide(g) == Reason::kOffThisSession); }
  { StartFacts g = f; g.failed_this_version = true; CHECK(Decide(g) == Reason::kFailedBefore); }
  { StartFacts g = f; g.files_ok = false; g.shaders_prepared = false; CHECK(Decide(g) == Reason::kNoFiles); }
  for (int r = 0; r <= int(Reason::kFailedBefore); ++r) CHECK(ReasonText(Reason(r))[0] != '?');

  // The failure marker: only this version's "starting" or "failed" keeps the stills.
  CHECK(ReadMarker("starting 2.0.0-beta.12\n", "2.0.0-beta.12") == MarkerState::kFailedThisVersion);
  CHECK(ReadMarker("failed 2.0.0-beta.12\r\n", "2.0.0-beta.12") == MarkerState::kFailedThisVersion);
  CHECK(ReadMarker("starting 2.0.0-beta.11\n", "2.0.0-beta.12") == MarkerState::kNone);
  CHECK(ReadMarker("", "2.0.0-beta.12") == MarkerState::kNone);
  CHECK(ReadMarker("garbage", "2.0.0-beta.12") == MarkerState::kNone);

  // Start on the title: from the 10th read, 3 reads down in every 20.
  CHECK(!PressStartOnTitle(0) && !PressStartOnTitle(9));
  CHECK(PressStartOnTitle(10) && PressStartOnTitle(12) && !PressStartOnTitle(13) && !PressStartOnTitle(29));
  CHECK(PressStartOnTitle(30));

  // The tracker: the title presses Start until the start menu is asked for, then nothing; ready after the
  // start menu has been up for kReadyReads reads; leaving the start menu is not ready.
  Tracker t;
  int presses = 0;
  for (int i = 0; i < 40; ++i) {
    t.Step({kTitleLevel, false, 0});
    presses += t.start_pressed;
  }
  CHECK(presses == 6);  // reads 10-12 and 30-32
  t.Step({kTitleLevel, true, kStartMenuLevel});
  CHECK(!t.start_pressed);
  for (int i = 0; i < 50; ++i) {
    t.Step({kTitleLevel, true, kStartMenuLevel});
    CHECK(!t.start_pressed);
  }
  for (int i = 0; i < kReadyReads - 1; ++i) t.Step({kStartMenuLevel, false, 0});
  CHECK(!t.Ready());
  t.Step({kStartMenuLevel, false, 0});
  CHECK(t.Ready());
  t.Step({kStartMenuLevel, true, 100});  // something else loading: the stills again
  CHECK(!t.Ready());
  t.Step({1, false, 0});  // the universe level (boot): no presses, not ready
  CHECK(!t.start_pressed && !t.Ready());

  // The scene's frame rate: the player's cap up to 60, unlimited as 60, 10 while throttled.
  CHECK(SceneFrameCap(30, false) == 30 && SceneFrameCap(60, false) == 60 && SceneFrameCap(144, false) == 60);
  CHECK(SceneFrameCap(0, false) == 60 && SceneFrameCap(-1, false) == 60);
  CHECK(SceneFrameCap(30, true) == 10 && SceneFrameCap(0, true) == 10);

  // The mix: 0.6 s each way, frame times clamped.
  float m = 0;
  for (int i = 0; i < 36; ++i) m = StepMix(m, true, 1.0f / 60);
  CHECK(m > 0.99f);
  m = StepMix(m, false, 5.0f);  // a long frame moves at most 0.1 s worth
  CHECK(m > 0.8f);
  for (int i = 0; i < 40; ++i) m = StepMix(m, false, 1.0f / 60);
  CHECK(m == 0.0f);

  if (failures) return 1;
  std::printf("menu scene tests passed\n");
  return 0;
}
