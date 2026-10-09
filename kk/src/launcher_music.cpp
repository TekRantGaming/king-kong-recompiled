#include "launcher_music.h"

#include <algorithm>

#include <SDL3/SDL.h>

#include <rex/logging.h>

#include "game_sound.h"

namespace kk {
namespace {

// The main menu's music: a 4-minute stereo stream (it starts at the title and
// plays through the save menu and the main menu), found by watching which
// part of Sound_Common.bf the game streams there.
constexpr uint32_t kMenuMusicKey = 0x060002C9;

}  // namespace

LauncherMusic::LauncherMusic(std::filesystem::path game_dir) {
  worker_ = std::thread([this, dir = std::move(game_dir)] { Load(dir); });
}

LauncherMusic::~LauncherMusic() {
  if (worker_.joinable()) worker_.join();
  if (stream_) SDL_DestroyAudioStream(stream_);
  if (audio_ready_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void LauncherMusic::Load(std::filesystem::path game_dir) {
  GameSound music = std::move(ReadGameSounds(game_dir, {kMenuMusicKey})[0]);
  pcm_ = std::move(music.pcm);
  channels_ = music.channels;
  rate_ = music.rate;
  ready_ = !pcm_.empty();
}

// Keeps about two seconds queued, from the start again at the end.
void LauncherMusic::Feed() {
  const int frame_bytes = channels_ * 2;
  const int want = rate_ * 2 * frame_bytes;
  while (SDL_GetAudioStreamQueued(stream_) < want) {
    const size_t chunk = std::min<size_t>(pcm_.size() - next_, size_t(rate_) * channels_ / 2);
    SDL_PutAudioStreamData(stream_, &pcm_[next_], int(chunk * 2));
    next_ = (next_ + chunk) % pcm_.size();
  }
}

void LauncherMusic::Update(float volume, float dt) {
  if (!ready_) return;
  if (!stream_) {
    if (volume <= 0) return;  // switched off: don't open the device at all
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      ready_ = false;
      return;
    }
    audio_ready_ = true;
    const SDL_AudioSpec spec{SDL_AUDIO_S16LE, channels_, rate_};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) {
      REXLOG_WARN("KK: launcher music: no audio device ({})", SDL_GetError());
      ready_ = false;
      return;
    }
    SDL_SetAudioStreamGain(stream_, 0);
    SDL_ResumeAudioStreamDevice(stream_);
    REXLOG_INFO("KK: launcher music: {:.0f} s at {} Hz", double(pcm_.size()) / channels_ / rate_, rate_);
  }
  // Fade in (or out) over about a second and a half.
  const float step = dt / 1.5f;
  gain_ = volume > gain_ ? std::min(volume, gain_ + step) : std::max(volume, gain_ - step);
  SDL_SetAudioStreamGain(stream_, gain_);
  Feed();
}

}  // namespace kk
