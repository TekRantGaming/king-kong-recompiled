#include "launcher_sounds.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>

#include <SDL3/SDL.h>

#include <rex/logging.h>

#include "game_sound.h"

namespace kk {
namespace {

constexpr int kRate = 48000;

// The main menu's sounds in Sound_Common.bf, found by tracing what the game
// plays there (its slot table while the menu is open): moving between
// entries, choosing one, and going back.
constexpr uint32_t kMoveKey = 0x87003BF2, kSelectKey = 0x87003BF4, kBackKey = 0x87003D8A;
enum Clip { kMoveClip, kSelectClip, kBackClip };

// Which of the game's sounds each launcher action uses, at what pitch and
// level. The game has no sounds of its own for switches, sliders or tabs, so
// those borrow its three at other pitches.
struct Use {
  Clip clip;
  double pitch;
  float gain;
};
constexpr Use kUses[] = {
    {kMoveClip, 1.0, 1.0f},     // kMove
    {kSelectClip, 1.0, 1.0f},   // kSelect
    {kBackClip, 1.0, 1.0f},     // kBack
    {kSelectClip, 1.0, 0.8f},   // kOn
    {kBackClip, 1.0, 0.8f},     // kOff
    {kMoveClip, 1.08, 1.0f},    // kChange
    {kMoveClip, 1.0, 0.6f},     // kSlide (pitch from the value)
    {kMoveClip, 0.88, 1.0f},    // kTab
    {kBackClip, 0.75, 0.9f},    // kDeny
};
static_assert(std::size(kUses) == size_t(ui::Sound::kCount));

double Now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

LauncherSounds::LauncherSounds(const std::filesystem::path& game_dir) {
  const auto sounds = ReadGameSounds(game_dir, {kMoveKey, kSelectKey, kBackKey});
  for (size_t i = 0; i < clips_.size(); ++i) {
    const GameSound& s = sounds[i];
    if (s.pcm.empty()) continue;
    // To mono floats; the mixer resamples as it plays.
    const size_t frames = s.pcm.size() / size_t(s.channels);
    clips_[i].pcm.resize(frames);
    for (size_t f = 0; f < frames; ++f) {
      float sum = 0;
      for (int c = 0; c < s.channels; ++c) sum += s.pcm[f * size_t(s.channels) + size_t(c)];
      clips_[i].pcm[f] = sum / (32768.0f * float(s.channels));
    }
    clips_[i].rate = s.rate;
  }
  if (clips_[kMoveClip].pcm.empty()) {
    REXLOG_WARN("KK: launcher sounds: the game's menu sounds weren't found");
    return;
  }
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
  audio_ready_ = true;
  const SDL_AudioSpec spec{SDL_AUDIO_F32, 1, kRate};
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &LauncherSounds::Feed, this);
  if (!stream_) {
    REXLOG_WARN("KK: launcher sounds: no audio device ({})", SDL_GetError());
    return;
  }
  SDL_ResumeAudioStreamDevice(stream_);
}

LauncherSounds::~LauncherSounds() {
  if (stream_) SDL_DestroyAudioStream(stream_);
  if (audio_ready_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void LauncherSounds::Play(ui::Sound sound, float value, float volume) {
  const size_t i = size_t(sound);
#if defined(KK_DEV_TOOLS)
  static const bool log = std::getenv("KK_DEV_SOUNDS_LOG") != nullptr;
  if (log) REXLOG_INFO("KK dev: sound {} ({:.2f}, volume {:.2f})", i, value, volume);
#endif
  if (!stream_ || i >= std::size(kUses) || volume <= 0) return;
  const Use& use = kUses[i];
  const Clip& clip = clips_[use.clip];
  if (clip.pcm.empty()) return;
  // Held buttons and mouse sweeps: not more than one of a sound every 40 ms.
  const double now = Now();
  if (now - last_[i] < 0.04) return;
  last_[i] = now;
  double pitch = use.pitch;
  if (sound == ui::Sound::kSlide) pitch = 0.85 + 0.35 * std::clamp(value, 0.0f, 1.0f);
  std::lock_guard<std::mutex> hold(lock_);
  if (voices_.size() >= 8) voices_.erase(voices_.begin());
  voices_.push_back({&clip, 0.0, clip.rate / kRate * pitch, volume * use.gain});
}

// The audio thread asks for more: mixes the sounds that are playing.
void LauncherSounds::Feed(void* user, SDL_AudioStream* stream, int additional, int) {
  auto* self = static_cast<LauncherSounds*>(user);
  const int frames = additional / int(sizeof(float));
  if (frames <= 0) return;
  std::lock_guard<std::mutex> hold(self->lock_);
  if (self->voices_.empty()) return;  // silence
  self->mix_.assign(size_t(frames), 0.0f);
  for (Voice& v : self->voices_) {
    const std::vector<float>& pcm = v.clip->pcm;
    for (int f = 0; f < frames && v.pos + 1 < double(pcm.size()); ++f) {
      const size_t a = size_t(v.pos);
      const float t = float(v.pos - double(a));
      self->mix_[size_t(f)] += v.gain * (pcm[a] + (pcm[a + 1] - pcm[a]) * t);
      v.pos += v.step;
    }
  }
  std::erase_if(self->voices_, [](const Voice& v) { return v.pos + 1 >= double(v.clip->pcm.size()); });
  for (float& x : self->mix_) x = std::clamp(x, -1.0f, 1.0f);
  SDL_PutAudioStreamData(stream, self->mix_.data(), frames * int(sizeof(float)));
}

}  // namespace kk
