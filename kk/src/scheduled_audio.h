#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/logging.h>
#include "backend/thread_scheduling.h"

namespace kk {

// Initialize runs on the SDK's Audio Worker, before its semaphore-driven pump.
class ScheduledAudioSystem : public rex::audio::sdl::SDLAudioSystem {
 public:
  using SDLAudioSystem::SDLAudioSystem;
  static std::unique_ptr<rex::audio::AudioSystem> Create(rex::runtime::FunctionDispatcher* dispatcher) {
    return std::make_unique<ScheduledAudioSystem>(dispatcher);
  }

  // Task 285: the game thread opens the audio device here (the game's XAudio client registration, under
  // the audio system's global lock), between "D3D hooks connected" and "audio endpoint ..." in the log.
  // It normally takes up to about a second (a virtual device such as SteelSeries Sonar is slow); one
  // restarted game in ~20 starts (REPORT-271) drew no first frame and logged nothing after "D3D hooks
  // connected". If the open ever takes long again, the log names it.
  rex::X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                        rex::audio::AudioDriver** out_driver) override {
    auto done = std::make_shared<std::atomic<bool>>(false);
    const auto start = std::chrono::steady_clock::now();
    std::thread([done] {
      for (int s = 1; s <= 60 && !done->load(); ++s) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!done->load() && (s == 10 || s == 30 || s == 60))
          REXLOG_WARN("KK audio: still waiting for the audio device to open after {} s (SDL audio / Windows "
                      "audio endpoint, on the game thread; the game does not go on until it opens)", s);
      }
    }).detach();
    const rex::X_STATUS status = SDLAudioSystem::CreateDriver(index, semaphore, out_driver);
    done->store(true);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (ms >= 2000.0) REXLOG_WARN("KK audio: opening the audio device took {:.1f} s", ms / 1000.0);
    return status;
  }

 protected:
  void Initialize() override {
    nr::ScheduleCriticalThread(true);
    SDLAudioSystem::Initialize();
  }
};

}  // namespace kk
