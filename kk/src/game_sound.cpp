#include "game_sound.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace kk {
namespace {

uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// SoundHeaders.db lists each sound with its key, where it is in
// Sound_Common.bf and its format: big-endian words key, type, offset, size,
// then a wave format (0x69 Xbox ADPCM, channels, rate, bytes per second,
// block size 36 per channel, 4 bits). Entries vary in length, so this finds
// the format words and reads the four words before them.
struct Entry {
  uint32_t offset = 0, size = 0, channels = 0, rate = 0;
};

// Xbox ADPCM (IMA ADPCM): per channel a 36-byte block of a 4-byte header
// (starting sample, step index) and 64 four-bit samples; with two channels the
// data alternates 4 bytes (8 samples) of each.
constexpr int16_t kSteps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr int kIndexStep[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

std::vector<int16_t> DecodeXboxAdpcm(const uint8_t* data, size_t size, int channels) {
  const size_t block = size_t(36) * channels, blocks = size / block;
  std::vector<int16_t> out(blocks * 64 * channels);
  for (size_t b = 0; b < blocks; ++b) {
    const uint8_t* blk = data + b * block;
    int pred[2], index[2];
    for (int c = 0; c < channels; ++c) {
      pred[c] = int16_t(blk[c * 4] | blk[c * 4 + 1] << 8);
      index[c] = std::min<int>(blk[c * 4 + 2], 88);
    }
    int16_t* frame = &out[b * 64 * channels];
    for (int chunk = 0; chunk < 8; ++chunk) {
      for (int c = 0; c < channels; ++c) {
        const uint8_t* bytes = blk + 4 * channels + (chunk * channels + c) * 4;
        for (int i = 0; i < 8; ++i) {
          const int nibble = (bytes[i / 2] >> ((i & 1) * 4)) & 15;
          const int step = kSteps[index[c]];
          int diff = step >> 3;
          if (nibble & 1) diff += step >> 2;
          if (nibble & 2) diff += step >> 1;
          if (nibble & 4) diff += step;
          if (nibble & 8) diff = -diff;
          pred[c] = std::clamp(pred[c] + diff, -32768, 32767);
          index[c] = std::clamp(index[c] + kIndexStep[nibble & 7], 0, 88);
          frame[(chunk * 8 + i) * channels + c] = int16_t(pred[c]);
        }
      }
    }
  }
  return out;
}

}  // namespace

std::vector<GameSound> ReadGameSounds(const std::filesystem::path& game_dir, const std::vector<uint32_t>& keys) {
  std::vector<GameSound> out(keys.size());
  std::ifstream db_file(game_dir / "Sound" / "SoundHeaders.db", std::ios::binary);
  const std::vector<uint8_t> db((std::istreambuf_iterator<char>(db_file)), std::istreambuf_iterator<char>());
  std::vector<Entry> entries(keys.size());
  for (size_t p = 16; p + 24 <= db.size(); p += 4) {
    if (BE32(&db[p]) != 0x69) continue;
    const uint32_t channels = BE32(&db[p + 4]), rate = BE32(&db[p + 8]), block = BE32(&db[p + 16]);
    if ((channels != 1 && channels != 2) || rate < 8000 || rate > 48000 || block != 36 * channels) continue;
    const auto it = std::find(keys.begin(), keys.end(), BE32(&db[p - 16]));
    if (it != keys.end()) entries[size_t(it - keys.begin())] = {BE32(&db[p - 8]), BE32(&db[p - 4]), channels, rate};
  }
  std::ifstream bank(game_dir / "Sound" / "Sound_Common.bf", std::ios::binary);
  for (size_t i = 0; i < keys.size(); ++i) {
    const Entry& e = entries[i];
    if (!e.size) continue;
    std::vector<uint8_t> data(e.size);
    bank.seekg(std::streamoff(e.offset));
    if (!bank.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()))) {
      bank.clear();
      continue;
    }
    out[i] = {DecodeXboxAdpcm(data.data(), data.size(), int(e.channels)), int(e.channels), int(e.rate)};
  }
  return out;
}

}  // namespace kk
