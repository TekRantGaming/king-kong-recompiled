// Task 342: slow device open, silent pacing, exact handoff and late completion.
#include "deferred_audio.h"

#include <atomic>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <vector>

using namespace std::chrono_literals;
using Driver = kk::DeferredAudioDriver;
namespace {
void Check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
template<class F> void Until(F predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!predicate()) {
    Check(std::chrono::steady_clock::now() < deadline, "worker timed out");
    std::this_thread::sleep_for(1ms);
  }
}
class Tokens final : public rex::thread::Semaphore {
 public:
  void* native_handle() const override { return nullptr; }
  bool Release(int n, int*) override { returned += n; return true; }
  std::atomic<int> returned{0};
};
struct Observed {
  std::atomic<int> submitted{0}, destroyed{0}, ready{0}, closed{0};
  std::atomic<uint32_t> sample{0};
};
class Fake final : public rex::audio::AudioDriver {
 public:
  Fake(rex::thread::Semaphore* sem, std::shared_ptr<Observed> observed)
      : AudioDriver(nullptr), sem_(sem), observed_(std::move(observed)) {}
  ~Fake() override {
    // A final SDL callback can race client destruction. The gate must survive
    // and report success even after its target semaphore has been disconnected.
    Check(sem_->Release(1, nullptr), "late callback must be safe");
    ++observed_->destroyed;
  }
  void SubmitFrame(uint32_t p) override {
    observed_->sample = p;
    ++observed_->submitted;
    Check(sem_->Release(1, nullptr), "completion must reach the SDK directly");
  }
 private:
  rex::thread::Semaphore* sem_;
  std::shared_ptr<Observed> observed_;
};
void Delete(rex::audio::AudioDriver* p) { delete p; }
Driver::Ready OnReady(std::shared_ptr<Observed> o) {
  return [o](rex::audio::AudioDriver* p) { if (p) ++o->ready; else ++o->closed; };
}
}  // namespace

int main() {
  try {
    {
      Tokens tokens;
      auto o = std::make_shared<Observed>();
      std::promise<void> finish;
      auto gate = finish.get_future().share();
      const auto caller = std::this_thread::get_id();
      const auto began = std::chrono::steady_clock::now();
      auto driver = std::make_unique<Driver>(nullptr, &tokens,
          [gate, o, caller](rex::thread::Semaphore* sem) {
            Check(std::this_thread::get_id() != caller, "open must use another thread");
            gate.wait();
            return Driver::Backend(new Fake(sem, o), Delete);
          }, OnReady(o));
      // The opener is held indefinitely. Construction and submissions return;
      // a finite number of real-time silence completions follows.
      for (int i = 0; i < 8; ++i) driver->SubmitFrame(0x1000 + i);
      Until([&] { return tokens.returned.load() == 8; });
      Check(std::chrono::steady_clock::now() - began >= 20ms,
            "silent completions must be paced, not returned in a busy loop");
      Check(o->submitted == 0, "pending samples must not reach unopened device");
      std::this_thread::sleep_for(20ms);
      Check(tokens.returned == 8, "idle client must not create semaphore credit");
      for (int i = 0; i < 8; ++i) driver->SubmitFrame(0x2000 + i);
      finish.set_value();
      Until([&] { return o->ready.load() == 1; });
      // Publication and token handoff are under one lock; SubmitFrame waits for
      // that small critical section, never for the outstanding device open.
      driver->SubmitFrame(0xCAFE);
      Check(o->submitted == 1 && o->sample == 0xCAFE, "only current PCM goes to ready driver");
      Check(tokens.returned == 17, "silent and real frames return exactly one token each");
      driver.reset();
      Until([&] { return o->destroyed.load() == 1; });
      Check(o->closed == 1 && tokens.returned == 17, "disconnect precedes backend cleanup");
    }
    // Release the entire client while its device open is still pending, then
    // finish opening. This used to require waiting on the opening thread.
    for (int repetition = 0; repetition < 40; ++repetition) {
      auto o = std::make_shared<Observed>();
      auto tokens = std::make_unique<Tokens>();
      std::promise<void> finish;
      auto gate = finish.get_future().share();
      auto driver = std::make_unique<Driver>(nullptr, tokens.get(),
          [gate, o](rex::thread::Semaphore* sem) {
            gate.wait();
            return Driver::Backend(new Fake(sem, o), Delete);
          }, OnReady(o));
      driver->SubmitFrame(123);
      driver.reset();
      tokens.reset();
      finish.set_value();
      Until([&] { return o->destroyed.load() == 1; });
      Check(o->submitted == 0 && o->ready == 0 && o->closed == 0,
            "cancelled open must never publish or submit to its backend");
    }
    {
      Tokens tokens;
      auto driver = std::make_unique<Driver>(nullptr, &tokens,
          [](rex::thread::Semaphore*) { return Driver::Backend(nullptr, Delete); });
      for (int i = 0; i < 8; ++i) driver->SubmitFrame(i);
      Until([&] { return tokens.returned.load() == 8; });
      Check(tokens.returned == 8, "no device must still pace the guest mixer");
    }
    std::puts("PASS: deferred audio open, handoff, failure and 40 cancelled lifetimes");
    return 0;
  } catch (const std::exception& e) {
    std::printf("FAIL: %s\n", e.what());
    return 1;
  }
}
