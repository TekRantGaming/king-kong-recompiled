// Task 332: the startup movies (startup_movies.h).
#include "startup_movies.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <string>

#include <SDL3/SDL.h>

#include <rex/logging.h>

#include "loading_loop.h"
#include "ps2_movie_decoder.h"
#include "startup_movies_logic.h"

namespace kk {

// A movie's whole sound, decoded on a worker while its first picture is made.
struct StartupMovies::Sound {
  std::mutex mutex;
  std::vector<uint8_t> pcm;  // 48 kHz 16-bit stereo (ps2_movie_decoder.h)
  bool done = false;         // decoding finished (pcm empty: no sound)
  std::atomic<bool> cancel{false};
};

StartupMovies::StartupMovies(rex::ui::ImmediateDrawer* drawer, std::vector<std::filesystem::path> files)
    : drawer_(drawer), files_(std::move(files)) {
  REXLOG_INFO("KK startup movies: {} to play", files_.size());
  Open();
}

StartupMovies::~StartupMovies() {
  Close();
  if (stream_) SDL_DestroyAudioStream(stream_);
  if (audio_ready_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void StartupMovies::Close() {
  if (sound_) sound_->cancel = true;
  if (sound_thread_.joinable()) sound_thread_.join();
  sound_.reset();
  video_.reset();
  if (stream_) SDL_ClearAudioStream(stream_);
  playing_ = false;
  own_clock_ = 0;
  sound_bytes_ = 0;
  started_logged_ = false;
}

// The movie at index_ (or the next one that can be played): its picture and its sound start decoding now.
void StartupMovies::Open() {
  for (; index_ < files_.size(); ++index_) {
    const std::filesystem::path& file = files_[index_];
    std::error_code ec;
    if (!movies::DecoderAvailable()) {
      REXLOG_WARN("KK startup movies: this build has no video decoder; none played");
      index_ = files_.size();
      return;
    }
    if (!std::filesystem::is_regular_file(file, ec)) {
      REXLOG_WARN("KK startup movies: {} is missing; left out", file.string());
      continue;
    }
    video_ = std::make_unique<LoadingLoop>(drawer_, file, /*once=*/true);
    sound_ = std::make_shared<Sound>();
    sound_thread_ = std::thread([sound = sound_, file] {
      std::vector<uint8_t> pcm;
      try {
        pcm = movies::DecodeAudio(file, sound->cancel);
      } catch (const std::exception& e) {
        REXLOG_INFO("KK startup movies: {}: no sound ({})", file.filename().string(), e.what());
        pcm.clear();
      }
      std::lock_guard lock(sound->mutex);
      sound->pcm = std::move(pcm);
      sound->done = true;
    });
    return;
  }
}

void StartupMovies::StartSound() {
  std::vector<uint8_t> pcm;
  {
    std::lock_guard lock(sound_->mutex);
    pcm.swap(sound_->pcm);
  }
  if (pcm.empty()) return;
  if (!stream_) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      REXLOG_WARN("KK startup movies: no sound ({})", SDL_GetError());
      return;
    }
    audio_ready_ = true;
    const SDL_AudioSpec spec{SDL_AUDIO_S16LE, movies::kAudioChannels, movies::kAudioRate};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) {
      REXLOG_WARN("KK startup movies: no audio device ({})", SDL_GetError());
      return;
    }
    SDL_ResumeAudioStreamDevice(stream_);
  }
  SDL_PutAudioStreamData(stream_, pcm.data(), int(pcm.size()));
  sound_bytes_ = int64_t(pcm.size());
}

rex::ui::ImmediateTexture* StartupMovies::Frame(float dt, bool go) {
  if (Done() || !video_) return nullptr;
  if (!playing_) {
    // The first picture and the sound are both ready (or the movie cannot be played): it starts, picture and sound
    // together.
    bool sound_done;
    {
      std::lock_guard lock(sound_->mutex);
      sound_done = sound_->done;
    }
    const bool can_start = video_->Ended() || (video_->HasFirstFrame() && sound_done);
    if (!go || !can_start) return video_->Frame(0.0f, 0.0);  // its first picture, held (or black before it)
    playing_ = true;
    StartSound();
  }
  own_clock_ += std::clamp(double(dt), 0.0, 0.1);
  double clock = own_clock_;
  if (stream_ && sound_bytes_ > 0) {
    const int queued = SDL_GetAudioStreamQueued(stream_);
    const double heard = double(sound_bytes_ - std::max(queued, 0)) /
                         double(movies::kAudioRate * movies::kAudioChannels * 2);
    clock = startup_movies::Clock(own_clock_, heard, queued > 0);
    if (queued > 0) own_clock_ = heard;  // carried on from the sound once it has played out
  }
  rex::ui::ImmediateTexture* frame = video_->Frame(float(dt), clock);
  if (frame && !started_logged_) {
    started_logged_ = true;
    REXLOG_INFO("KK startup movies: playing {} ({}{})", files_[index_].filename().string(), movies::DecoderName(),
                sound_bytes_ > 0 ? ", with sound" : ", no sound");
  }
  if (video_->Ended()) {
    REXLOG_INFO("KK startup movies: {} {}", files_[index_].filename().string(),
                frame ? "ended" : "could not be played; left out");
    Close();  // its last picture stays valid for this frame (LoadingLoop keeps its texture)
    ++index_;
    Open();
    if (Done()) REXLOG_INFO("KK startup movies: all played");
  }
  return frame;
}

float StartupMovies::Aspect() const { return video_ ? video_->Aspect() : 16.0f / 9.0f; }

}  // namespace kk
