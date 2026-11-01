// Whether the game the player installed is the disc this port was translated from. The launcher only reads the title
// ID, which other copies share. Their code differs, so the game dies at once on a function the port does not have
// (crash reports of 31 Oct: "Call to invalid or unregistered function at guest address 0x828824C0"). The hash of the
// loaded code tells them apart whatever file or dump it came from; the crash handler then says why (crash_dump.cpp).
// It stops nothing by itself: a copy that differs but runs keeps running.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace kk {

inline constexpr uint32_t kGameCodeStart = 0x820D0000, kGameCodeEnd = 0x828981A0;
inline constexpr uint64_t kGameCodeHash = 0;

inline std::atomic<bool> g_game_code_differs{false};

inline uint64_t HashGameCode(const uint8_t* code, size_t size) {
  uint64_t hash = 14695981039346656037ull;
  for (size_t i = 0; i + 8 <= size; i += 8) {
    uint64_t word;
    std::memcpy(&word, code + i, 8);
    hash = (hash ^ word) * 1099511628211ull;
  }
  return hash;
}

}  // namespace kk
