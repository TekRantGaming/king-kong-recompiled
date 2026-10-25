// Task 330: the live main menu scene's rules (menu_scene_logic.h).
#include <cstdio>
#include <cstdlib>

#include "menu_scene_logic.h"
#include "loading_loop_logic.h"
#include "startup_movies_logic.h"

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
  // Task 349c (owner, 21 Oct): another renderer runs the scene too (its shaders are not prepared by the launcher).
  { StartFacts g = f; g.native_renderer = false; CHECK(Decide(g) == Reason::kLive); }
  { StartFacts g = f; g.native_renderer = false; g.shaders_prepared = false; CHECK(Decide(g) == Reason::kLive); }
  { StartFacts g = f; g.shaders_prepared = false; CHECK(Decide(g) == Reason::kShadersNotPrepared); }
  { StartFacts g = f; g.shader_job = true; CHECK(Decide(g) == Reason::kShaderJob); }
  { StartFacts g = f; g.setup = true; CHECK(Decide(g) == Reason::kSetup); }
  { StartFacts g = f; g.off_this_session = true; CHECK(Decide(g) == Reason::kOffThisSession); }
  { StartFacts g = f; g.failed_this_version = true; CHECK(Decide(g) == Reason::kFailedBefore); }
  { StartFacts g = f; g.files_ok = false; g.shaders_prepared = false; CHECK(Decide(g) == Reason::kNoFiles); }
  // A fresh install, an update or a Rebuild: the scene starts later in the same launcher; only giving up does not.
  CHECK(MayStartLater(Reason::kNoFiles) && MayStartLater(Reason::kShadersNotPrepared) &&
        MayStartLater(Reason::kShaderJob) && MayStartLater(Reason::kSetup) && MayStartLater(Reason::kOffThisSession));
  CHECK(!MayStartLater(Reason::kLive) && !MayStartLater(Reason::kFailedBefore));
  for (int r = 0; r <= int(Reason::kFailedBefore); ++r) CHECK(ReasonText(Reason(r))[0] != '?');

  // The failure marker (task 349c): one failed start is counted, not given up on; 3 in a row give up for the
  // version; the second attempt of the same start does not count again; another version's is nothing.
  {
    const char* v = "2.0.0-beta.12";
    auto is = [](MarkerRead r, int n, bool gave_up) { return r.failed_starts == n && r.gave_up == gave_up; };
    CHECK(is(ReadMarker("starting 2.0.0-beta.12\n", v), 1, false));  // Beta 13 style, no count
    CHECK(is(ReadMarker("starting 2.0.0-beta.12 0\r\n", v), 1, false));
    CHECK(is(ReadMarker("starting 2.0.0-beta.12 1", v), 2, false));
    CHECK(is(ReadMarker("starting 2.0.0-beta.12 2", v), 3, true));
    CHECK(is(ReadMarker("starting 2.0.0-beta.12 0", v, true), 0, false));
    CHECK(is(ReadMarker("starting 2.0.0-beta.12 2", v, true), 2, false));
    CHECK(is(ReadMarker("failed 2.0.0-beta.12\r\n", v), 0, true));
    CHECK(is(ReadMarker("starting 2.0.0-beta.11 2\n", v), 0, false));
    CHECK(is(ReadMarker("failed 2.0.0-beta.11", v), 0, false));
    CHECK(is(ReadMarker("starting 2.0.0-beta.12 x", v), 0, false));
    CHECK(is(ReadMarker("", v), 0, false) && is(ReadMarker("garbage", v), 0, false));
    CHECK(StartingMarker(v, 2) == "starting 2.0.0-beta.12 2");
    CHECK(is(ReadMarker(StartingMarker(v, 0), v), 1, false));
  }

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

  // Task 331: the loading loops. Which one: the notice from the desktop, Returning back from the game, none for a
  // launcher restarted for a shader rebuild.
  {
    using namespace kk::loading_loop;
    CHECK(KindForStart("") == Kind::kNotice);
    CHECK(KindForStart("play") == Kind::kReturning && KindForStart("chapters") == Kind::kReturning);
    CHECK(KindForStart("extras") == Kind::kReturning && KindForStart("benchmark") == Kind::kReturning);
    CHECK(KindForStart("rebuild") == Kind::kNone);
    // At least 3 s; then the scene (when one was started) or, without one, at once. Task 349c: a scene not up 30 s
    // after its start: a second attempt; the pictures only when that one is not up in 30 s either.
    CHECK(Step(2.9f, false, false, 0, false) == SceneStep::kWait && Step(3.0f, false, false, 0, false) == SceneStep::kEnd);
    CHECK(Step(2.9f, true, true, 5, false) == SceneStep::kWait && Step(3.1f, true, true, 5, false) == SceneStep::kEnd);
    CHECK(Step(20.0f, true, false, 20, false) == SceneStep::kWait && Step(29.0f, true, false, 29.9f, true) == SceneStep::kWait);
    CHECK(Step(28.0f, true, false, 30, false) == SceneStep::kRetry && Step(28.0f, true, false, 30, true) == SceneStep::kPictures);
    CHECK(Step(1.0f, true, false, 90, false) == SceneStep::kWait);  // after the startup movies: 3 s of the loop first
    CHECK(Step(40.0f, true, true, 40, true) == SceneStep::kEnd);
    CHECK(Cover(-1.0f) == 1.0f && Cover(0.0f) == 1.0f && Cover(0.25f) > 0.4f && Cover(0.25f) < 0.6f && Cover(0.5f) == 0.0f);
    // The tips go round: 0 first, the one after the last, back to 0 after the last one; junk starts again.
    CHECK(NextTip("", 12) == 0 && NextTip("0", 12) == 1 && NextTip("11", 12) == 0 && NextTip("5", 12) == 6);
    CHECK(NextTip("x", 12) == 0 && NextTip("-3", 12) == 0 && NextTip("40", 12) == 5 && NextTip("3", 0) == 0);
  }

  // Task 332: the startup movies. After the setup they always play (a pop-up first with skip on); later only from
  // the desktop, with the game files and no setup screen first, and not with kk_skip_intros on.
  {
    using namespace kk::startup_movies;
    CHECK(Play(true, true) && Play(true, false) && Play(false, false) && !Play(false, true));
    CHECK(AskFirst(true, true) && !AskFirst(true, false) && !AskFirst(false, true) && !AskFirst(false, false));
    CHECK(AtLauncherStart("", true, false, false));
    CHECK(!AtLauncherStart("", true, false, true));    // skip intros on
    CHECK(!AtLauncherStart("play", true, false, false) && !AtLauncherStart("rebuild", true, false, false));
    CHECK(!AtLauncherStart("", false, false, false));  // no game files yet
    CHECK(!AtLauncherStart("", true, true, false));    // a setup or update screen comes first
    const auto files = Files("G", "P");
    CHECK(files.size() == 4);
    CHECK(files[0].filename() == "Ubisoft.wmv" && files[1].filename() == "Universal.wmv" &&
          files[2].filename() == "Wingnut.wmv" && files[3].filename() == "trg-startup.mp4");
    CHECK(files[0].parent_path().filename() == "Video" && files[3].parent_path().filename() == "video");
    CHECK(Clock(1.0, 2.0, true) == 2.0 && Clock(1.0, 2.0, false) == 1.0);
  }

  if (failures) return 1;
  std::printf("menu scene tests passed\n");
  return 0;
}
