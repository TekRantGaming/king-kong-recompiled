// Developer-only helpers for scripted launcher and gameplay tests (never shipped
// behaviour, but header-only so the launcher tests can pin them).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace kk {
// KK_DEV_LAUNCHER_INPUT may hold several launcher sessions separated by '|':
// "extras-save:1,pad:down,pad:a|pad:down,pad:down,pad:a". The launcher runs only
// the first; starting a bonus hands the rest to the program that comes back to
// Extras afterwards, so one scripted run can try several bonuses in turn.
inline std::string_view DevLauncherSession(std::string_view all) {
  const size_t bar = all.find('|');
  return bar == std::string_view::npos ? all : all.substr(0, bar);
}
// What the launcher input becomes once a bonus has started: the returned page's
// marker, then the next session, if any.
inline std::string DevLauncherAfterStart(std::string_view all) {
  std::string next = "shot:returned-extras";
  const size_t bar = all.find('|');
  if (bar != std::string_view::npos && bar + 1 < all.size()) {
    next += ',';
    next += all.substr(bar + 1);
  }
  return next;
}

// KK_DEV_SCRIPT button names (XINPUT button bits, triggers as bits 16/17, the
// right stick 18-22, SKIP 23, the left stick 24-28). Unknown names give 0.
inline constexpr uint32_t kDevLT = 1u << 16, kDevRT = 1u << 17;
inline constexpr uint32_t kDevRsUp = 1u << 18, kDevRsDown = 1u << 19, kDevRsLeft = 1u << 20,
                          kDevRsRight = 1u << 21, kDevRsHalf = 1u << 22, kDevSkip = 1u << 23;
inline constexpr uint32_t kDevLsUp = 1u << 24, kDevLsDown = 1u << 25, kDevLsLeft = 1u << 26,
                          kDevLsRight = 1u << 27, kDevLsHalf = 1u << 28;
inline constexpr uint32_t kDevFreecam = 1u << 29;  // switches the free camera (dev_tools.cpp)
inline uint32_t DevScriptButton(std::string_view s) {
  struct Name {
    std::string_view name;
    uint32_t bit;
  };
  static constexpr Name kNames[] = {
      {"UP", 0x0001},        {"DOWN", 0x0002},        {"LEFT", 0x0004},        {"RIGHT", 0x0008},
      {"START", 0x0010},     {"BACK", 0x0020},        {"LS", 0x0040},          {"RS", 0x0080},
      {"LB", 0x0100},        {"RB", 0x0200},          {"A", 0x1000},           {"B", 0x2000},
      {"X", 0x4000},         {"Y", 0x8000},           {"LT", kDevLT},          {"RT", kDevRT},
      {"RSUP", kDevRsUp},    {"RSDOWN", kDevRsDown},  {"RSLEFT", kDevRsLeft},  {"RSRIGHT", kDevRsRight},
      {"RSHALF", kDevRsHalf}, {"SKIP", kDevSkip},     {"LSUP", kDevLsUp},      {"LSDOWN", kDevLsDown},
      {"LSLEFT", kDevLsLeft}, {"LSRIGHT", kDevLsRight}, {"LSHALF", kDevLsHalf}, {"FREECAM", kDevFreecam}};
  for (const auto& n : kNames)
    if (s == n.name) return n.bit;
  return 0;
}
// The four stick axes (left X, left Y, right X, right Y) for held script buttons.
struct DevSticks {
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
inline DevSticks DevScriptSticks(uint32_t held) {
  DevSticks s;
  const int16_t left = (held & kDevLsHalf) ? 16384 : 32767;
  const int16_t right = (held & kDevRsHalf) ? 16384 : 32767;
  if (held & (kDevLsLeft | kDevLsRight)) s.lx = (held & kDevLsLeft) ? int16_t(-left) : left;
  if (held & (kDevLsUp | kDevLsDown)) s.ly = (held & kDevLsDown) ? int16_t(-left) : left;
  if (held & (kDevRsLeft | kDevRsRight)) s.rx = (held & kDevRsLeft) ? int16_t(-right) : right;
  if (held & (kDevRsUp | kDevRsDown)) s.ry = (held & kDevRsDown) ? int16_t(-right) : right;
  return s;
}
}  // namespace kk
