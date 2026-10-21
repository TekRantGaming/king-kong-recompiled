#include "background_log_sink.h"
#include <spdlog/sinks/base_sink.h>
#include <chrono>
#include <future>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace std::chrono_literals;
namespace {
int failures = 0;
void Check(bool ok, const char* text) {
  if (!ok) { std::printf("FAIL: %s\n", text); ++failures; }
}
std::string_view View(spdlog::string_view_t text) { return {text.data(), text.size()}; }
class RecordingSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  std::vector<spdlog::details::log_msg_buffer> lines;
  int flushes = 0;
  std::binary_semaphore entered{0}, release{0};
  bool block_first = false;
 private:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    if (block_first && lines.empty()) { entered.release(); release.acquire(); }
    lines.emplace_back(msg);
  }
  void flush_() override { ++flushes; }
};
void OwnedMessagesAndFlush() {
  auto output = std::make_shared<RecordingSink>();
  output->block_first = true;
  kk::BackgroundLogSink background({output}, spdlog::level::info, 256);
  const auto now = spdlog::log_clock::now();
  for (int i = 0; i < 100; ++i) {
    std::string category = "original-category", payload = "message " + std::to_string(i);
    spdlog::details::log_msg msg(now, {"source.cpp", 12, "producer"}, category, spdlog::level::info, payload);
    msg.thread_id = 1234;
    background.log(msg);
    category.assign(100, 'x'); payload.assign(100, 'x');
    if (!i) output->entered.acquire();
  }
  // The worker is blocked in output, yet the producer queued every message.
  auto flush = std::async(std::launch::async, [&] { background.flush(); });
  Check(flush.wait_for(20ms) == std::future_status::timeout, "explicit flush waits for preceding output");
  output->release.release();
  flush.get();
  Check(output->lines.size() == 100, "no messages lost while output was blocked");
  for (int i = 0; i < int(output->lines.size()); ++i) {
    const auto& msg = output->lines[i];
    Check(View(msg.payload) == "message " + std::to_string(i), "owned payload and FIFO ordering");
    Check(View(msg.logger_name) == "original-category" && msg.thread_id == 1234 && msg.time == now,
          "original category, thread and timestamp retained");
    Check(msg.source.line == 12 && std::string(msg.source.filename) == "source.cpp", "source retained");
  }
  Check(output->flushes == 101, "original automatic flush level plus explicit flush");
  background.Stop();
  spdlog::details::log_msg final({}, "final", spdlog::level::info, "hard exit follows");
  background.log(final);
  Check(output->lines.size() == 101 && View(output->lines.back().payload) == "hard exit follows",
        "runtime's final output is synchronous after clean exit drain");
  background.Stop();
}
void FullQueueAndStop() {
  auto output = std::make_shared<RecordingSink>();
  output->block_first = true;
  kk::BackgroundLogSink background({output}, spdlog::level::off, 2);
  auto producer = std::async(std::launch::async, [&] {
    for (int i = 0; i < 100; ++i) {
      const std::string payload = std::to_string(i);
      background.log({{}, "full", spdlog::level::info, payload});
    }
  });
  output->entered.acquire();
  Check(producer.wait_for(20ms) == std::future_status::timeout, "bounded queue applies backpressure without drops");
  auto stop = std::async(std::launch::async, [&] { background.Stop(); });
  output->release.release();
  stop.get(); producer.get();
  Check(output->lines.size() == 100, "stop wakes producers and preserves all queued and final messages");
  for (int i = 0; i < int(output->lines.size()); ++i)
    Check(View(output->lines[i].payload) == std::to_string(i), "FIFO through stop and direct fallback");
}
class ThrowingSink final : public spdlog::sinks::base_sink<std::mutex> {
  void sink_it_(const spdlog::details::log_msg&) override { throw std::runtime_error("test output failure"); }
  void flush_() override { throw std::runtime_error("test flush failure"); }
};
void FailedOutput() {
  auto output = std::make_shared<RecordingSink>();
  kk::BackgroundLogSink background({std::make_shared<ThrowingSink>(), output}, spdlog::level::off);
  background.log({{}, "error", spdlog::level::info, "healthy sink still receives this"});
  background.flush();
  background.Stop();
  Check(output->lines.size() == 1 && output->flushes >= 1, "one failed sink does not stop other output or drain");
}
}
int main() {
  OwnedMessagesAndFlush();
  FullQueueAndStop();
  FailedOutput();
  std::printf("background log output: %d failures\n", failures);
  return failures ? 1 : 0;
}
