// Window/device-free parts of the F8 panel, shared with its test harness.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace kk::panel {
struct PadState {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  bool Neutral() const {
    return !buttons && lt <= 30 && rt <= 30 &&
           std::abs(int(lx)) <= 8000 && std::abs(int(ly)) <= 8000 &&
           std::abs(int(rx)) <= 8000 && std::abs(int(ry)) <= 8000;
  }
};

// Runs at the game's pad read, before remapping. Neither single stick click is
// reserved. The simultaneous chord has no separate game binding. Suppress its
// first sample too, and after closing require neutral before returning to play.
struct PadCapture {
  static constexpr uint16_t kChord = 0x00C0;
  bool chord_down = false, toggle = false, release = false;
  PadState latest;
  bool Filter(PadState& state, bool in_play, bool open) {
    latest = state;
    const bool chord = (state.buttons & kChord) == kChord;
    if (in_play && chord && !chord_down) toggle = true;
    chord_down = chord;
    const bool capture = open || toggle;
    if (capture) release = true;
    else if (state.Neutral()) release = false;
    if (capture || release) {
      state = {};
      return true;
    }
    return false;
  }
  bool TakeToggle() { const bool out = toggle; toggle = false; return out; }
};

struct UpscalerCaps {
  bool known = false, dlss = false, fsr4 = false, fsr3 = false, xess = false;
};
struct UpscalerChoice { const char* label; const char* value; };
inline constexpr UpscalerChoice kUpscalers[] = {
    {"Off", "off"}, {"NVIDIA DLSS", "dlss"}, {"AMD FSR 4", "fsr4"}, {"AMD FSR 3.1", "fsr3"}, {"Intel XeSS", "xess"}};
// Indices into the panel's fixed choice table. FSR 4 requires a positive SDK
// answer as well as the active DirectX 12 device being AMD.
inline std::vector<int> OfferedUpscalers(uint32_t api, uint32_t vendor, UpscalerCaps c,
                                       bool windows = true) {
  std::vector<int> out{0};
  if (!windows) return out;
  if (c.known ? c.dlss : vendor == 0x10DE) out.push_back(1);
  if (api == 2 && vendor == 0x1002 && c.known && c.fsr4) out.push_back(2);
  if (!c.known || c.fsr3) out.push_back(3);
  if (!c.known || c.xess) out.push_back(4);
  return out;
}

// An unavailable saved value is a preview only; navigation enters the offered
// choices without inserting that unavailable value into the list.
inline int NextChoice(int current, int count, int direction) {
  if (count <= 0) return -1;
  if (current < 0 || current >= count) return direction > 0 ? 0 : count - 1;
  return (current + count + direction) % count;
}
}  // namespace kk::panel
