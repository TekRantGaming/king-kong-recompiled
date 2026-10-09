// Sounds from the player's copy of the game: Sound/SoundHeaders.db says where
// each one is in Sound/Sound_Common.bf and its format (Xbox ADPCM).

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace kk {

struct GameSound {
  std::vector<int16_t> pcm;  // interleaved; empty when not found
  int channels = 0, rate = 0;
};

// Reads and decodes the sounds with these keys (one pass over the headers).
std::vector<GameSound> ReadGameSounds(const std::filesystem::path& game_dir, const std::vector<uint32_t>& keys);

}  // namespace kk
