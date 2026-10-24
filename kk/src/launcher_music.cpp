#include "launcher_music.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>

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

// The opener retains this state if the launcher closes while Windows is still
// opening the device. It never calls back into a destroyed LauncherMusic.
struct LauncherMusic::Device {
  std::atomic<bool> ready{false};
  SDL_AudioStream* stream = nullptr;
  bool initialized = false;
  ~Device() {
    if (stream) SDL_DestroyAudioStream(stream);
    if (initialized) SDL_QuitSubSystem(SDL_INIT_AUDIO);
  }
};

LauncherMusic::LauncherMusic(std::filesystem::path game_dir) {
  worker_ = std::thread([this, dir = std::move(game_dir)] { Load(dir); });
}

LauncherMusic::~LauncherMusic() {
  if (worker_.joinable()) worker_.join();
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
  auto* stream_ = device_->stream;
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
  if (!device_) {
    if (volume <= 0) return;  // switched off: don't open the device at all
    device_ = std::make_shared<Device>();
    std::thread([device = device_, channels = channels_, rate = rate_] {
      const auto start = std::chrono::steady_clock::now();
#if defined(KK_DEV_TOOLS)
      if (const char* d = std::getenv("KK_DEV_AUDIO_OPEN_DELAY"); d && std::atoi(d) > 0)
        std::this_thread::sleep_for(std::chrono::seconds(std::atoi(d)));
#endif
      device->initialized = SDL_InitSubSystem(SDL_INIT_AUDIO);
      if (device->initialized) {
        const SDL_AudioSpec spec{SDL_AUDIO_S16LE, channels, rate};
        device->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
      }
      if (device->stream) {
        SDL_SetAudioStreamGain(device->stream, 0);
        SDL_ResumeAudioStreamDevice(device->stream);
        REXLOG_INFO("KK: launcher music: device ready on worker after {:.3f} s",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
      } else REXLOG_WARN("KK: launcher music: no audio device ({})", SDL_GetError());
      device->ready.store(true, std::memory_order_release);
    }).detach();
    REXLOG_INFO("KK: launcher music: device open queued on worker");
  }
  if (!device_->ready.load(std::memory_order_acquire) || !device_->stream) return;
  auto* stream_ = device_->stream;
  // Fade in (or out) over about a second and a half.
  const float step = dt / 1.5f;
  gain_ = volume > gain_ ? std::min(volume, gain_ + step) : std::max(volume, gain_ - step);
  SDL_SetAudioStreamGain(stream_, gain_);
  // Faded out (switched off, or a shader build): the device pauses, and carries on where it was.
  const bool silent = gain_ <= 0 && volume <= 0;
  if (silent != paused_) {
    paused_ = silent;
    if (silent) SDL_PauseAudioStreamDevice(stream_);
    else SDL_ResumeAudioStreamDevice(stream_);
  }
  if (!paused_) Feed();
}

}  // namespace kk
