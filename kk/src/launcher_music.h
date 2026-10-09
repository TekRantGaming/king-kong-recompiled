// The game's main menu music, played by the launcher from the player's own
// game files (Sound/Sound_Common.bf, found through Sound/SoundHeaders.db).

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <vector>

struct SDL_AudioStream;

namespace kk {

class LauncherMusic {
 public:
  // Starts reading and decoding the track on a worker thread.
  explicit LauncherMusic(std::filesystem::path game_dir);
  ~LauncherMusic();

  // Once a frame: starts playing when the track is ready, keeps it looping,
  // and fades to `volume` (0..1; 0 when switched off).
  void Update(float volume, float dt);

 private:
  void Load(std::filesystem::path game_dir);
  void Feed();

  std::thread worker_;
  std::atomic<bool> ready_{false};
  std::vector<int16_t> pcm_;  // interleaved
  int channels_ = 2, rate_ = 24000;
  SDL_AudioStream* stream_ = nullptr;
  bool audio_ready_ = false;
  size_t next_ = 0;  // next sample to queue (loops)
  float gain_ = 0;
};

}  // namespace kk
