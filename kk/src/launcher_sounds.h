// The launcher's menu sounds: the game's own front-end sounds (move, select,
// back) from the player's copy, mixed on their own audio stream so quick
// presses overlap.

#pragma once

#include <array>
#include <filesystem>
#include <mutex>
#include <vector>

#include "launcher_ui.h"

struct SDL_AudioStream;

namespace kk {

class LauncherSounds {
 public:
  // Reads the sounds from the installed game and opens the audio device.
  explicit LauncherSounds(const std::filesystem::path& game_dir);
  ~LauncherSounds();

  // `value`: where a slider is (0..1), which sets its pitch. `volume`: 0..1.
  void Play(ui::Sound sound, float value, float volume);

 private:
  struct Clip {
    std::vector<float> pcm;  // mono
    double rate = 0;
  };
  struct Voice {
    const Clip* clip;
    double pos, step;
    float gain;
  };
  static void Feed(void* self, SDL_AudioStream* stream, int additional, int total);

  std::array<Clip, 3> clips_;  // move, select, back
  std::array<double, size_t(ui::Sound::kCount)> last_{};
  std::mutex lock_;
  std::vector<Voice> voices_;
  std::vector<float> mix_;  // the audio thread's
  SDL_AudioStream* stream_ = nullptr;
  bool audio_ready_ = false;
};

}  // namespace kk
