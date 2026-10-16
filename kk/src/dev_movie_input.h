// Playback detection for the developer pad's movie-only SKIP action.
#pragma once
#include <cstdint>

namespace kk {
template <typename Read32>
bool DevMoviePlaying(Read32 read) {
  const uint32_t player = read(0x82CCAE80);
  // Story movies use +0x10C/+0x110; the separate Extras player uses +0x130.
  return player && (read(player + 0x10C) || read(player + 0x110) || read(player + 0x130));
}
}  // namespace kk
