// Task 211 (worst-frame detail, telemetry.h): running totals of the game's own file reads (its ReadFile,
// sub_82106DE0, io_fix.cpp), from every thread. A few relaxed atomic additions per read; the performance
// summary reads them at each frame and keeps them only for the slow frames.
#pragma once

#include <atomic>
#include <cstdint>

namespace kk::iostats {

struct Counters {
  uint64_t reads = 0, video_reads = 0, bytes = 0, us = 0;
  uint64_t longest_us = 0;  // the longest single read since the previous Take
};

inline std::atomic<uint64_t> g_reads{0}, g_video_reads{0}, g_bytes{0}, g_us{0}, g_longest_us{0};

// One read: the bytes asked for, the time the call took, and whether the video player made it.
inline void NoteRead(uint32_t bytes, uint64_t us, bool video) {
  g_reads.fetch_add(1, std::memory_order_relaxed);
  if (video) g_video_reads.fetch_add(1, std::memory_order_relaxed);
  g_bytes.fetch_add(bytes, std::memory_order_relaxed);
  g_us.fetch_add(us, std::memory_order_relaxed);
  uint64_t seen = g_longest_us.load(std::memory_order_relaxed);
  while (us > seen && !g_longest_us.compare_exchange_weak(seen, us, std::memory_order_relaxed)) {
  }
}

// The totals now; the longest read starts again from 0 (one reader: the performance summary, per frame).
inline Counters Take() {
  Counters c;
  c.reads = g_reads.load(std::memory_order_relaxed);
  c.video_reads = g_video_reads.load(std::memory_order_relaxed);
  c.bytes = g_bytes.load(std::memory_order_relaxed);
  c.us = g_us.load(std::memory_order_relaxed);
  c.longest_us = g_longest_us.exchange(0, std::memory_order_relaxed);
  return c;
}

}  // namespace kk::iostats
