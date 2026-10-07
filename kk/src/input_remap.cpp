// Controller remapping and stick inversion.
//
// sub_821074E8 is the title's XInputGetState(user, state) wrapper (it calls
// XamInputGetState) and the only place it reads the pad. After the real call
// we rewrite the guest XINPUT_STATE (big-endian):
//   +0 dwPacketNumber, +4 wButtons, +6 bLeftTrigger, +7 bRightTrigger,
//   +8 sThumbLX, +10 sThumbLY, +12 sThumbRX, +14 sThumbRY.
// Keyboard input arrives through the same path when ReXGlue's mnk_mode is on,
// so remaps and inversion apply to it too.
//
// Camera sensitivity is applied later, to the stick vector the game asks for
// (sub_8272C610), so it is not capped at a full push of the stick.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "settings.h"

namespace {

constexpr uint8_t kTriggerPressThreshold = 30;  // XINPUT_GAMEPAD_TRIGGER_THRESHOLD

template <typename T>
T LoadBE(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof(v));
  return std::byteswap(v);
}

template <typename T>
void StoreBE(uint8_t* p, T v) {
  v = std::byteswap(v);
  std::memcpy(p, &v, sizeof(v));
}

void Invert(uint8_t* p) {
  const int16_t v = LoadBE<int16_t>(p);
  StoreBE<int16_t>(p, v == INT16_MIN ? INT16_MAX : static_cast<int16_t>(-v));
}

// Radial deadzone (rescaled so output still starts at 0) and a gain, applied to
// one stick's X/Y pair. The gain is capped at a full push.
void ShapeStick(uint8_t* xy, float deadzone, float gain) {
  if (deadzone <= 0.0f && gain == 1.0f) return;
  float x = LoadBE<int16_t>(xy) / 32767.0f, y = LoadBE<int16_t>(xy + 2) / 32767.0f;
  const float mag = std::sqrt(x * x + y * y);
  if (mag <= deadzone || mag == 0.0f) {
    x = y = 0.0f;
  } else {
    const float scaled = std::min(1.0f, (mag - deadzone) / (1.0f - deadzone) * gain);
    x = x / mag * scaled;
    y = y / mag * scaled;
  }
  StoreBE<int16_t>(xy, static_cast<int16_t>(std::clamp(x, -1.0f, 1.0f) * 32767.0f));
  StoreBE<int16_t>(xy + 2, static_cast<int16_t>(std::clamp(y, -1.0f, 1.0f) * 32767.0f));
}

// Toggle aim (kk_toggle_aim): each press of the game's left trigger flips it
// between released and fully held, per pad. Start or Back (the pause menu and
// map) and unplugging the pad release it, so a menu never opens mid-aim and
// the gun is lowered when play resumes.
struct AimToggle {
  bool held = false;      // what the game is told
  bool was_down = false;  // the trigger last poll, to catch new presses
};
AimToggle g_aim[4];

uint8_t ToggleAim(uint32_t user, uint8_t lt, uint16_t buttons) {
  AimToggle& aim = g_aim[user & 3];
  if (!REXCVAR_GET(kk_toggle_aim)) {
    aim = {};
    return lt;
  }
  const bool down = lt > kTriggerPressThreshold;
  if (down && !aim.was_down) aim.held = !aim.held;
  aim.was_down = down;
  constexpr uint16_t kStart = 0x0010, kBack = 0x0020;
  if (buttons & (kStart | kBack)) aim.held = false;
  return aim.held ? 255 : 0;
}

void Remap(uint32_t user, uint8_t* state) {
  using kk::Pad;
  const uint16_t in_buttons = LoadBE<uint16_t>(state + 4);
  const uint8_t in_lt = state[6], in_rt = state[7];

  uint16_t out_buttons = 0;
  uint8_t out_lt = 0, out_rt = 0;
  for (size_t i = 0; i < kk::kPadCount; ++i) {
    const auto physical = static_cast<Pad>(i);
    uint8_t analog;  // 0..255 strength of the physical control
    if (physical == Pad::kLeftTrigger) {
      analog = in_lt;
    } else if (physical == Pad::kRightTrigger) {
      analog = in_rt;
    } else {
      analog = (in_buttons & kk::GetPadInfo(physical).mask) ? 255 : 0;
    }
    if (analog == 0) continue;

    const Pad target = kk::GetMapping(physical);
    if (target == Pad::kNone) continue;
    if (target == Pad::kLeftTrigger) {
      out_lt = std::max(out_lt, analog);
    } else if (target == Pad::kRightTrigger) {
      out_rt = std::max(out_rt, analog);
    } else if (analog > kTriggerPressThreshold) {
      out_buttons |= kk::GetPadInfo(target).mask;
    }
  }
  StoreBE<uint16_t>(state + 4, out_buttons);
  state[6] = ToggleAim(user, out_lt, out_buttons);
  state[7] = out_rt;

  if (REXCVAR_GET(kk_invert_ls_x)) Invert(state + 8);
  if (REXCVAR_GET(kk_invert_ls_y)) Invert(state + 10);
  if (REXCVAR_GET(kk_invert_rs_x)) Invert(state + 12);
  if (REXCVAR_GET(kk_invert_rs_y)) Invert(state + 14);

  const float deadzone = std::clamp(REXCVAR_GET(kk_deadzone), 0, 50) / 100.0f;
  ShapeStick(state + 8, deadzone, 1.0f);
  ShapeStick(state + 12, deadzone, 1.0f);
}

}  // namespace

namespace kk {
// Set by developer builds (dev_tools.cpp) to feed in pad input; returns true
// when it filled in the state.
bool (*g_dev_pad_input)(uint32_t user, uint8_t* state) = nullptr;
}  // namespace kk

REX_EXTERN(__imp__sub_821074E8);
REX_HOOK_RAW(sub_821074E8) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t state_ptr = ctx.r4.u32;
  __imp__sub_821074E8(ctx, base);
  if (kk::g_dev_pad_input && state_ptr && kk::g_dev_pad_input(user, base + state_ptr)) {
    ctx.r3.u64 = 0;  // ERROR_SUCCESS: a pad is connected
    return;
  }
  if (ctx.r3.u32 == 0 && state_ptr) {  // ERROR_SUCCESS
    Remap(user, base + state_ptr);
  } else {
    g_aim[user & 3] = {};  // no pad: let go of a toggled aim
  }
}

// sub_821074F8 is the title's XInputSetState(user, vibration) wrapper; scale or
// drop the guest XINPUT_VIBRATION {u16 left, u16 right} before it is applied.
REX_EXTERN(__imp__sub_821074F8);
REX_HOOK_RAW(sub_821074F8) {
  if (const uint32_t vib = ctx.r4.u32) {
    const float strength =
        REXCVAR_GET(kk_vibration) ? std::clamp(REXCVAR_GET(kk_vibration_strength), 0, 100) / 100.0f : 0.0f;
    for (uint32_t offset : {0u, 2u}) {
      uint8_t* motor = base + vib + offset;
      StoreBE<uint16_t>(motor, static_cast<uint16_t>(LoadBE<uint16_t>(motor) * strength));
    }
  }
  __imp__sub_821074F8(ctx, base);
}

// sub_8272C610(out, stick) is how the game reads a stick: out = {x, y, 0} as
// floats, about -1 to 1, the strongest of the pads, after the game's own
// inversion options; stick 1 is the right stick, which turns the camera and
// moves the aim. Controller sensitivity scales that vector here, after the
// stick's full range, so a full push turns faster or slower too and both axes
// change together. With keyboard & mouse the mouse has its own sensitivity.
REX_EXTERN(__imp__sub_8272C610);
REX_HOOK_RAW(sub_8272C610) {
  const uint32_t out = ctx.r3.u32, stick = ctx.r4.u32;
  __imp__sub_8272C610(ctx, base);
  if (stick != 1 || !out || rex::cvar::GetFlagByName("mnk_mode") == "true") return;
  const float gain = std::clamp(REXCVAR_GET(kk_camera_sensitivity), 10, 400) / 100.0f;
  if (gain == 1.0f) return;
  for (uint32_t offset : {0u, 4u}) {
    uint8_t* v = base + out + offset;
    StoreBE<uint32_t>(v, std::bit_cast<uint32_t>(std::bit_cast<float>(LoadBE<uint32_t>(v)) * gain));
  }
}
