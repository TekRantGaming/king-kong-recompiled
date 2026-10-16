// The original retail Extras catalogue and unlock rules (IntMIG_Page_Xtra,
// sub_825E6188 / sub_825E5DD8). Names and hints are read from the player's files.
#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <span>

namespace kk::play {
enum class ExtraType { kLevel, kMovie, kMode };
struct ExtraInfo {
  int id;
  ExtraType type;
  uint32_t level;        // original level key or movie string's guest address
  int score;
  int progress;          // percent, -1 no progress requirement; >100 online-code bonus
  uint32_t start_point = 0;
  uint32_t session_flag = 0;  // value written to G+0x5674, alternate ending only
  uint32_t mode_offset = 0;   // G-relative option field; modes do not load a level
};
// Targets from sub_824CBFE8's id switch, not the order of its branch labels.
inline constexpr std::array<ExtraInfo, 15> kExtras = {{
    {19, ExtraType::kMovie, 0x820AE520, -1, -1},
    {18, ExtraType::kLevel, 7, -1, -1},
    {12, ExtraType::kLevel, 54, -1, 5},
    {1, ExtraType::kMode, 0, -1, 15, 0, 0, 0x4DA4},
    {15, ExtraType::kLevel, 55, -1, 1000},
    {9, ExtraType::kLevel, 57, -1, 50},
    {5, ExtraType::kLevel, 50, -1, 75},
    {8, ExtraType::kMode, 0, -1, 100, 0, 0, 0x4DA0},
    {6, ExtraType::kLevel, 51, 20000, -1},
    {7, ExtraType::kLevel, 56, 50000, -1},
    {0, ExtraType::kMovie, 0x820AE4F0, 75000, -1},
    {10, ExtraType::kLevel, 52, 100000, -1},
    {11, ExtraType::kLevel, 53, 150000, -1},
    {3, ExtraType::kMovie, 0x820AE508, -1, 2000},
    {2, ExtraType::kLevel, 2003, 250000, 100, 1, 1},
}};
inline constexpr const ExtraInfo* FindExtra(int id) {
  for (const auto& e : kExtras) if (e.id == id) return &e;
  return nullptr;
}
struct ExtraProgress {
  int percent = 0;
  int64_t score = 0;
  uint32_t online_codes = 0;
  bool museum = false;
};
inline ExtraProgress DecodeExtraProgress(std::span<const uint8_t> bytes) {
  auto word = [&](size_t at) {
    return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 |
           uint32_t(bytes[at + 2]) << 8 | bytes[at + 3];
  };
  ExtraProgress p;
  if (bytes.size() != 0x8040 || !word(0x28) || word(0xA68) != 4) return p;
  p.percent = int(std::min(word(0x34), 41u)) * 100 / 41;
  for (size_t i = 0; i < 40; ++i) p.score += std::max(int32_t(word(0x38 + i * 4)), 0);
  // G+0x5E7C is saved at file+0x164; the twenty online-code words start at +4.
  for (size_t i = 0; i < 20; ++i) if (word(0x168 + 4 * i)) p.online_codes |= 1u << i;
  // KKmuseum is a temporary runtime flag, not part of the serialized save.
  return p;
}
inline bool ExtraUnlocked(const ExtraInfo& e, const ExtraProgress& p) {
  if (p.museum) return true;
  if (e.progress > 100) return (p.online_codes & (1u << e.id)) != 0;
  // The game truncates progress to a whole percent, then multiplies by 0.01f.
  return float(p.percent) * 0.01f >= float(e.progress) / 100.0f && p.score >= e.score;
}
}  // namespace kk::play
