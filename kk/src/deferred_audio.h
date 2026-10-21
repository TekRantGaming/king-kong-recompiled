#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include <rex/audio/audio_driver.h>
#include <rex/thread.h>

namespace kk {

// The device may take seconds to open or resume. Keep that operation, and its
// lifetime, separate from the game's audio client. After opening, every sample
// and semaphore signal goes through the original driver unchanged.
class DeferredAudioDriver final : public rex::audio::AudioDriver {
 public:
  using Backend = std::unique_ptr<rex::audio::AudioDriver, void (*)(rex::audio::AudioDriver*)>;
  using Open = std::function<Backend(rex::thread::Semaphore*)>;
  using Ready = std::function<void(rex::audio::AudioDriver*)>;
  using Waiting = std::function<void(int)>;

  DeferredAudioDriver(rex::memory::Memory* memory, rex::thread::Semaphore* semaphore,
                      Open open, Ready ready = {}, Waiting waiting = {})
      : AudioDriver(memory), state_(std::make_shared<State>(semaphore, std::move(ready))) {
    auto state = state_;
    std::thread([state, open = std::move(open)] {
      // Open owns its driver until publication. Cancellation never waits for
      // Windows, and a late completion cannot touch a destroyed client.
      auto driver = open(&state->completion);
      {
        std::lock_guard lock(state->mutex);
        if (state->cancelled) return;
        if (driver) {
          if (state->ready) state->ready(driver.get());
          // Silent frames have already passed in game time. Don't play them
          // late, and return all their tokens before the real driver takes over.
          if (state->pending) state->completion.Release(int(state->pending), nullptr);
          state->pending = 0;
          state->driver = std::move(driver);
        }
        state->opened = true;
      }
      state->cv.notify_all();
    }).detach();
    std::thread([state, waiting = std::move(waiting)] {
      using Clock = std::chrono::steady_clock;
      const auto start = Clock::now();
      // One guest audio frame is 256 samples at 48 kHz. An immediate release
      // from SubmitFrame would spin the guest mixer at unlimited speed.
      constexpr auto period = std::chrono::nanoseconds(256'000'000'000LL / 48000);
      auto next = start + period;
      int warned = 0;
      std::unique_lock lock(state->mutex);
      while (!state->cancelled && !state->driver) {
        state->cv.wait_until(lock, next, [&] { return state->cancelled || bool(state->driver); });
        if (state->cancelled || state->driver) break;
        const auto now = Clock::now();
        if (now < next) continue;
        // Bound catch-up by the frames actually submitted. An idle client must
        // not accumulate credit or release beyond the SDK semaphore's capacity.
        const auto ticks = 1 + (now - next) / period;
        next += period * ticks;
        const auto count = std::min<size_t>(state->pending, size_t(ticks));
        if (count) {
          state->pending -= count;
          state->completion.Release(int(count), nullptr);
        }
        const int seconds = int(std::chrono::duration_cast<std::chrono::seconds>(now - start).count());
        const int warning = seconds >= 60 ? 60 : seconds >= 30 ? 30 : seconds >= 10 ? 10 : 0;
        if (!state->opened && warning > warned) {
          warned = warning;
          lock.unlock();
          if (waiting) waiting(warning);
          lock.lock();
        }
      }
    }).detach();
  }

  ~DeferredAudioDriver() override {
    // The SDK may destroy its semaphore immediately after DestroyDriver. The
    // SDL callback uses this gate, whose lifetime also covers a pending open.
    {
      std::lock_guard lock(state_->mutex);
      state_->cancelled = true;
      if (state_->driver && state_->ready) state_->ready(nullptr);
    }
    state_->completion.Disconnect();
    state_->cv.notify_all();
  }

  void SubmitFrame(uint32_t samples) override {
    std::lock_guard lock(state_->mutex);
    if (state_->driver) state_->driver->SubmitFrame(samples);
    else ++state_->pending;
  }

 private:
  // SDLAudioDriver only calls Release on its semaphore. Forward it directly
  // while the client lives; after disconnect, acknowledge late callbacks
  // without referring to AudioSystem or its semaphore.
  class Completion final : public rex::thread::Semaphore {
   public:
    explicit Completion(rex::thread::Semaphore* target) : target_(target) {}
    void* native_handle() const override { return nullptr; }  // never waited on
    bool Release(int count, int* previous) override {
      std::lock_guard lock(mutex_);
      if (target_) return target_->Release(count, previous);
      if (previous) *previous = 0;
      return true;
    }
    void Disconnect() {
      std::lock_guard lock(mutex_);
      target_ = nullptr;
    }
   private:
    std::mutex mutex_;
    rex::thread::Semaphore* target_;
  };

  struct State {
    State(rex::thread::Semaphore* semaphore, Ready on_ready)
        : completion(semaphore), ready(std::move(on_ready)) {}
    // Driver destruction stops SDL callbacks before their completion gate dies.
    Completion completion;
    Ready ready;
    Backend driver{nullptr, [](rex::audio::AudioDriver*) {}};
    std::mutex mutex;
    std::condition_variable cv;
    size_t pending = 0;
    bool opened = false, cancelled = false;
  };
  std::shared_ptr<State> state_;
};

}  // namespace kk
