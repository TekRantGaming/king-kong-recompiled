#pragma once

#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>

#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include "backend/thread_scheduling.h"
#include "deferred_audio.h"
#include "menu_scene.h"

namespace kk {

// Initialize runs on the SDK's Audio Worker, before its semaphore-driven pump.
class ScheduledAudioSystem : public rex::audio::sdl::SDLAudioSystem {
 public:
  using SDLAudioSystem::SDLAudioSystem;
  static std::unique_ptr<rex::audio::AudioSystem> Create(rex::runtime::FunctionDispatcher* dispatcher) {
    return std::make_unique<ScheduledAudioSystem>(dispatcher);
  }

  // RegisterClient calls this on the game thread under the SDK's audio lock.
  // Device open AND resume may block (task 337: 10.3 s after the endpoint log).
  // Only queue the open here. DeferredAudioDriver paces the guest mixer with
  // silence until the unchanged SDK driver is ready, then submits current PCM.
  rex::X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                            rex::audio::AudioDriver** out_driver) override {
    if (!out_driver) return rex::X_STATUS(0xC0000001L);
    *out_driver = new DeferredAudioDriver(memory_, semaphore,
        [memory = memory_, index](rex::thread::Semaphore* completion) {
          rex::thread::set_current_thread_name("KK Audio Open");
          const auto start = std::chrono::steady_clock::now();
#if defined(KK_DEV_TOOLS)
          // Same task-286 test aid; the held operation now lives on its worker.
          if (const char* d = std::getenv("KK_DEV_AUDIO_OPEN_DELAY"); d && std::atoi(d) > 0) {
            REXLOG_INFO("KK dev: holding the audio device open for {} s", std::atoi(d));
            std::this_thread::sleep_for(std::chrono::seconds(std::atoi(d)));
          }
#endif
          DeferredAudioDriver::Backend driver(new rex::audio::sdl::SDLAudioDriver(memory, completion),
              [](rex::audio::AudioDriver* p) {
                auto* sdl = static_cast<rex::audio::sdl::SDLAudioDriver*>(p);
                sdl->Shutdown();
                delete sdl;
              });
          if (!static_cast<rex::audio::sdl::SDLAudioDriver*>(driver.get())->Initialize()) {
            REXLOG_WARN("KK audio: device open failed on worker (client {}); game continues silently", index);
            driver.reset();
            return driver;
          }
          const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
          REXLOG_INFO("KK audio: device ready after {:.3f} s on worker (client {})", seconds, index);
          return driver;
        },
        [](rex::audio::AudioDriver* driver) {
          // Bind before any real PCM is submitted; detach before destroying the
          // backend, including when cancellation races a successful open.
          if (kk::menu_scene::Active())
            kk::menu_scene::SetAudioStream(driver ? DriverStream::Of(
                static_cast<rex::audio::sdl::SDLAudioDriver*>(driver)) : nullptr);
        },
        [](int seconds) {
          REXLOG_WARN("KK audio: still waiting for the audio device to open after {} s "
                      "(SDL audio / Windows audio endpoint, on a worker; game continues silently)", seconds);
        });
    REXLOG_INFO("KK audio: device open queued on worker (client {}); game thread continues", index);
    return rex::X_STATUS(0);
  }

  void DestroyDriver(rex::audio::AudioDriver* driver) override {
    // A pending device open owns its completion gate and cleans up by itself.
    // No join on the game thread, and no late callback into an SDK semaphore.
    delete static_cast<DeferredAudioDriver*>(driver);
  }

 private:
  struct DriverStream : rex::audio::sdl::SDLAudioDriver {
    static SDL_AudioStream* Of(rex::audio::sdl::SDLAudioDriver* d) { return d->*(&DriverStream::sdl_stream_); }
  };

 protected:
  void Initialize() override {
    nr::SetCpuPriorityLevel(std::atoi(rex::cvar::GetFlagByName("kk_cpu_priority").c_str()));
    nr::ScheduleCriticalThread(true);
    SDLAudioSystem::Initialize();
  }
};

}  // namespace kk
