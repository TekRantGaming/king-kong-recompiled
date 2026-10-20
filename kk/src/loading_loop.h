// Task 331 (owner, 20 Oct): the owner's two loading loops, full screen over the launcher while the live menu scene
// starts behind it (menu_scene.h): the save notice when the game opens from the desktop, "Returning to the main
// menu" when the launcher comes back from the game. 1920x1080 H.264 MP4s, 4.6 s seamless loops with no sound,
// shipped beside the program in launcher\video. Decoded with the PS2 cutscenes' decoder (ps2_movie_decoder.h:
// Media Foundation on Windows, the shipped FFmpeg libraries on Linux) on a worker thread, a few frames ahead,
// and drawn frame by frame as a launcher texture. The rules (when it ends, the fade) are in loading_loop_logic.h.
// Task 332: the same player plays the startup movies once each (startup_movies.h: `once`, and a clock of their own).
#pragma once

#include <filesystem>
#include <memory>
#include <thread>

#include "loading_loop_logic.h"

namespace rex::ui {
class ImmediateDrawer;
class ImmediateTexture;
}  // namespace rex::ui

namespace kk {

class LoadingLoop {
 public:
  // Starts decoding `file` at once on a worker thread (so the first frame is there for the launcher's first frame).
  // `once` (task 332): the file plays once instead of again and again (Ended() says when it is over).
  LoadingLoop(rex::ui::ImmediateDrawer* drawer, std::filesystem::path file, bool once = false);
  ~LoadingLoop();
  LoadingLoop(const LoadingLoop&) = delete;
  LoadingLoop& operator=(const LoadingLoop&) = delete;

  // The file could not be played (no decoder in this build, missing or broken file): nothing to show.
  bool Failed() const;
  // UI thread, once a frame: moves the loop on by `dt` seconds (a long frame counts as 0.1 s, so the loop goes on
  // from where it stopped) and returns the frame due now as a texture (nullptr before the first one is decoded).
  // `clock` >= 0 (task 332): the time since the first frame from another clock (the movie's sound) instead of `dt`.
  rex::ui::ImmediateTexture* Frame(float dt, double clock = -1);
  // Once only: true when its last frame has had its time (or it cannot be played).
  bool Ended() const;
  // A first frame is decoded and waiting (or shown): the picture can start.
  bool HasFirstFrame() const;
  // Width / height of the picture (16:9 until the file is open).
  float Aspect() const;

  struct Shared;

 private:
  rex::ui::ImmediateDrawer* drawer_;
  std::shared_ptr<Shared> shared_;
  std::thread worker_;
  std::unique_ptr<rex::ui::ImmediateTexture> texture_;
  double position_ = 0;  // seconds on the loop's own timeline (it keeps counting across repeats)
  double first_at_ = -1;  // the first frame's time on it
  bool first_waited_ = false;
};

// <program folder>/launcher/video/<name>.
std::filesystem::path LoadingLoopFile(loading_loop::Kind kind);

}  // namespace kk
