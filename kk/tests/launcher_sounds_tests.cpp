// Exercise the production stream with made-up PCM; no game files or window.
#include "launcher_sounds.h"
#include "game_sound.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <SDL3/SDL.h>

namespace kk {
std::vector<GameSound> ReadGameSounds(const std::filesystem::path&, const std::vector<uint32_t>& keys) {
  GameSound clip;
  clip.channels = 1; clip.rate = 24000;
  clip.pcm.resize(clip.rate);
  for (int i = 0; i < clip.rate; ++i) clip.pcm[i] = int16_t(std::sin(i * 0.1) * 12000);
  return std::vector<GameSound>(keys.size(), clip);
}
struct LauncherSoundsTest {
  static void Run() {
    LauncherSounds sound;
    const SDL_AudioSpec spec{SDL_AUDIO_F32,1,48000};
    sound.stream_ = SDL_CreateAudioStream(&spec,&spec);
    if (!sound.stream_) throw std::runtime_error(std::string("no memory audio stream: ") + SDL_GetError());
    for (auto& clip : sound.clips_) {
      clip.rate = 24000;
      clip.pcm.resize(24000);
      for (int i=0;i<24000;++i) clip.pcm[i] = float(std::sin(i*0.1)*0.5);
    }
    sound.Play(ui::Sound::kSelect,0.5f,0.25f);
    if (SDL_GetAudioStreamGain(sound.stream_) != 0.25f || sound.voices_.size() != 1)
      throw std::runtime_error("menu volume did not reach production stream");
    if (sound.voices_[0].step != 0.5 || sound.voices_[0].gain != 1.0f)
      throw std::runtime_error("resampling or gain applied twice");
    LauncherSounds::Feed(&sound,sound.stream_,4096,0);
    float samples[1024];
    int bytes = SDL_GetAudioStreamData(sound.stream_,samples,sizeof(samples));
    float peak = 0;
    for (int i=0;i<bytes/int(sizeof(float));++i) peak = std::max(peak,std::abs(samples[i]));
    if (peak < 0.12f || peak > 0.126f) throw std::runtime_error("quarter-volume PCM amplitude is wrong");
    LauncherSounds::Feed(&sound,sound.stream_,4096,0); // already queued when the slider changes
    sound.SetVolume(0);
    if (SDL_GetAudioStreamGain(sound.stream_) != 0 || sound.voices_.empty())
      throw std::runtime_error("mute must affect an already playing voice");
    bytes = SDL_GetAudioStreamData(sound.stream_,samples,sizeof(samples));
    for (int i=0;i<bytes/int(sizeof(float));++i)
      if (samples[i] != 0) throw std::runtime_error("queued menu PCM did not mute");
    sound.SetVolume(1);
    if (SDL_GetAudioStreamGain(sound.stream_) != 1) throw std::runtime_error("full volume not restored");
    sound.SetVolume(-1);
    if (SDL_GetAudioStreamGain(sound.stream_) != 0) throw std::runtime_error("minimum clamp failed");
    sound.SetVolume(2);
    if (SDL_GetAudioStreamGain(sound.stream_) != 1) throw std::runtime_error("maximum clamp failed");
    sound.Play(ui::Sound::kMove,0.5f,0);
    if (SDL_GetAudioStreamGain(sound.stream_) != 0) throw std::runtime_error("zero cue did not mute existing voice");
  }
};
}
int main() {
  try { kk::LauncherSoundsTest::Run(); }
  catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
  std::cout << "menu sound live gain, mute, endpoints and resampling passed\n";
}
