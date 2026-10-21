// Task 339: file/debug output must not perform I/O on the guest draw thread.
// The caller still captures the diagnostic tail synchronously. This sink owns
// each queued message, including its original timestamp, category and thread.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include <spdlog/details/log_msg_buffer.h>
#include <spdlog/sinks/sink.h>

namespace kk {
class BackgroundLogSink final : public spdlog::sinks::sink {
 public:
  explicit BackgroundLogSink(std::vector<spdlog::sink_ptr> outputs,
                             spdlog::level::level_enum flush_level,
                             size_t capacity = 8192)
      : outputs_(std::move(outputs)), flush_level_(flush_level), capacity_(capacity ? capacity : 1),
        worker_([this] { Run(); }) {}
  ~BackgroundLogSink() override { Stop(); }

  void log(const spdlog::details::log_msg& msg) override {
    std::unique_lock lock(mutex_);
    room_.wait(lock, [this] { return direct_ || (!stopping_ && queue_.size() < capacity_); });
    if (direct_) {
      lock.unlock();
      Write(msg);
      return;
    }
    queue_.push_back({spdlog::details::log_msg_buffer(msg), false, 0});
    ready_.notify_one();
  }

  // Loggers' automatic flushes are performed by Write on the worker instead.
  // An explicit FlushLogging (including the SDK's periodic flusher) retains
  // its contract: all preceding messages have reached the original sinks.
  void flush() override {
    std::unique_lock lock(mutex_);
    room_.wait(lock, [this] { return direct_ || (!stopping_ && queue_.size() < capacity_); });
    if (direct_) {
      lock.unlock();
      FlushOutputs();
      return;
    }
    const auto ticket = ++requested_;
    queue_.push_back({{}, true, ticket});
    ready_.notify_one();
    drained_.wait(lock, [this, ticket] { return completed_ >= ticket; });
  }

  // Clean exit only. Drain once, then leave a direct sink in place so the
  // runtime's final messages before its hard exit are also written and flushed.
  void Stop() {
    std::unique_lock lock(mutex_);
    if (direct_) return;
    if (stopping_) {
      drained_.wait(lock, [this] { return direct_; });
      return;
    }
    stopping_ = true;
    ready_.notify_one();
    lock.unlock();
    worker_.join();
    lock.lock();
    direct_ = true;
    room_.notify_all();
    drained_.notify_all();
  }

  void set_pattern(const std::string& pattern) override {
    for (const auto& output : outputs_) output->set_pattern(pattern);
  }
  void set_formatter(std::unique_ptr<spdlog::formatter> formatter) override {
    for (const auto& output : outputs_) output->set_formatter(formatter->clone());
  }

 private:
  struct Item {
    spdlog::details::log_msg_buffer message;
    bool flush = false;
    uint64_t ticket = 0;
  };
  void FlushOutputs() {
    for (const auto& output : outputs_) Attempt([&] { output->flush(); });
  }
  void Write(const spdlog::details::log_msg& msg) {
    for (const auto& output : outputs_)
      if (output->should_log(msg.level)) Attempt([&] { output->log(msg); });
    if (msg.level >= flush_level_) FlushOutputs();
  }
  template <class F> static void Attempt(F&& write) {
    try { write(); }
    catch (const std::exception& error) { std::fprintf(stderr, "KK log output failed: %s\n", error.what()); }
    catch (...) { std::fputs("KK log output failed\n", stderr); }
  }
  void Run() {
    for (;;) {
      Item item;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (queue_.empty()) break;
        item = std::move(queue_.front());
        queue_.pop_front();
        room_.notify_all();
      }
      try {
        if (item.flush) FlushOutputs();
        else Write(item.message);
      } catch (const std::exception& error) {
        // As with spdlog's error handler, output failures do not stop the game.
        std::fprintf(stderr, "KK log output failed: %s\n", error.what());
      } catch (...) {
        std::fputs("KK log output failed\n", stderr);
      }
      if (item.flush) {
        std::lock_guard lock(mutex_);
        completed_ = item.ticket;
        drained_.notify_all();
      }
    }
    try { FlushOutputs(); } catch (...) {}
  }

  const std::vector<spdlog::sink_ptr> outputs_;
  const spdlog::level::level_enum flush_level_;
  const size_t capacity_;
  std::mutex mutex_;
  std::condition_variable ready_, room_, drained_;
  std::deque<Item> queue_;
  uint64_t requested_ = 0, completed_ = 0;
  bool stopping_ = false, direct_ = false;
  std::thread worker_;
};
}  // namespace kk
