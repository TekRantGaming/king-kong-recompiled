// The last log lines for crash and hang reports (Beta 7 uploads, task 197; codex/state/telemetry-b7.md wants
// "the last ~200 log lines" in each report). Every log line is redacted as for uploads (user names, personal
// folders: telemetry::RedactForUpload) and copied into a fixed ring of 200 lines of at most 512 bytes, all
// allocated up front, so the crash handler only reads fixed memory and calls a writer: no allocation, no
// blocking lock (a crash while a line is being copied reads the ring anyway; at worst that one line is mixed).
// Unit tested in tests/log_tail_tests.cpp. No platform calls here.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#include "telemetry_logic.h"

namespace kk {

// One log line as it may leave the PC: personal folders and names replaced (as the upload does), CR/LF
// removed, at most `max_bytes`. Cut before redacting (lines can be very long), with a margin the size of the
// longest thing redaction matches; when the line was cut, that margin's output is dropped too, so a name or
// home folder cut in half at the end never survives.
inline std::string RedactLogLine(std::string_view line, const telemetry::Private& personal, size_t max_bytes) {
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
  size_t reach = 16;  // "\var\home\" + a short name, "X:\Users\" + a short name
  for (const auto& s : personal.names) reach = std::max(reach, s.size() + 1);
  for (const auto& s : personal.homes) reach = std::max(reach, s.size() + 1);
  const bool cut = line.size() > max_bytes + reach;
  std::string text(cut ? line.substr(0, max_bytes + reach) : line);
  for (char& c : text)
    if (c == '\r' || c == '\n') c = ' ';
  text = telemetry::RedactForUpload(std::move(text), personal);
  if (cut) text.resize(text.size() - std::min(reach, text.size()));
  if (text.size() > max_bytes) text.resize(max_bytes);
  return text;
}

template <size_t kLines = 200, size_t kLineBytes = 512>
class LogTail {
 public:
  static constexpr size_t kMaxLines = kLines;
  static constexpr size_t kMaxLineBytes = kLineBytes;

  // Any thread (the log sink). Keeps the newest kLines; a longer line is cut to kLineBytes - 1 bytes.
  void Add(std::string_view line) {
    Lock();
    Slot& s = slots_[next_];
    const size_t n = std::min(line.size(), kLineBytes - 1);
    std::memcpy(s.text, line.data(), n);
    s.text[n] = 0;
    s.size = uint16_t(n);
    next_ = (next_ + 1) % kLines;
    if (count_ < kLines) ++count_;
    Unlock();
  }

  // Calls write(text, size) for each line, oldest first, then returns how many. Safe in a crash handler:
  // no allocation; it waits for a line being added only `spins` times, then reads anyway.
  template <class Write>
  size_t ForEach(Write&& write, int spins = 1 << 16) const {
    bool locked = false;
    for (int i = 0; i < spins && !(locked = TryLock()); ++i) std::this_thread::yield();
    const size_t count = std::min(count_, kLines);
    const size_t first = (next_ + kLines - count) % kLines;
    for (size_t i = 0; i < count; ++i) {
      const Slot& s = slots_[(first + i) % kLines];
      write(s.text, std::min<size_t>(s.size, kLineBytes - 1));
    }
    if (locked) Unlock();
    return count;
  }

  size_t Count() const {
    Lock();
    const size_t n = count_;
    Unlock();
    return n;
  }

  // For tests.
  std::string Text() const {
    std::string out;
    ForEach([&out](const char* text, size_t size) { out.append(text, size) += '\n'; });
    return out;
  }

 protected:  // the lock, for tests that hold it
  bool TryLock() const { return !busy_.test_and_set(std::memory_order_acquire); }
  void Lock() const {
    while (!TryLock()) std::this_thread::yield();
  }
  void Unlock() const { busy_.clear(std::memory_order_release); }

 private:
  struct Slot {
    char text[kLineBytes];
    uint16_t size;
  };
  static_assert(kLineBytes <= 65535 && kLines > 0);

  std::array<Slot, kLines> slots_{};
  size_t next_ = 0, count_ = 0;
  mutable std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
};

}  // namespace kk
