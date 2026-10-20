// Task 332 (owner, 20 Oct): the startup movies, full screen over the launcher, one after the other: the game's logo
// movies from the player's game files (Ubisoft, Universal, WingNut), then the owner's TRG Studios movie. The rules
// (when they play, the pop-up) are in startup_movies_logic.h. Each picture is decoded by the loading loops' player
// (loading_loop.h, once); a movie's sound (the logos have sound, the TRG movie has none) is decoded whole first with
// the PS2 cutscenes' decoder (ps2_movie_decoder.h) and played through the launcher's own SDL audio stream, and the
// picture follows the sound. A movie that cannot be played (Linux's FFmpeg build has no WMV decoder; a missing file)
// is left out and logged.
#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

struct SDL_AudioStream;

namespace rex::ui {
class ImmediateDrawer;
class ImmediateTexture;
}  // namespace rex::ui

namespace kk {

class LoadingLoop;

class StartupMovies {
 public:
  StartupMovies(rex::ui::ImmediateDrawer* drawer, std::vector<std::filesystem::path> files);
  ~StartupMovies();
  StartupMovies(const StartupMovies&) = delete;
  StartupMovies& operator=(const StartupMovies&) = delete;

  // UI thread, once a frame: the picture due now (nullptr: black). `go` false holds the first movie before it starts
  // (the live menu scene's start pauses the launcher once; the movies start after that).
  rex::ui::ImmediateTexture* Frame(float dt, bool go);
  float Aspect() const;
  // Every movie has played (or could not be).
  bool Done() const { return index_ >= files_.size(); }

  struct Sound;

 private:
  void Open();
  void Close();
  void StartSound();

  rex::ui::ImmediateDrawer* drawer_;
  std::vector<std::filesystem::path> files_;
  size_t index_ = 0;
  std::unique_ptr<LoadingLoop> video_;
  std::shared_ptr<Sound> sound_;
  std::thread sound_thread_;
  SDL_AudioStream* stream_ = nullptr;
  bool audio_ready_ = false;  // the SDL audio subsystem was opened (closed again with the movies)
  bool playing_ = false;      // the current movie has started (its first frame shown, its sound started)
  double own_clock_ = 0;      // seconds since the current movie started, by the launcher's frames
  int64_t sound_bytes_ = 0;   // the current movie's sound queued in all
  bool started_logged_ = false;
};

}  // namespace kk
