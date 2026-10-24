#pragma once
// C82: the player's controller while Steam runs this process as Spacewar (app 480).
//
// With Steam Input on for a controller (Steam's "Xbox configuration support", or any PlayStation/Switch pad),
// Steam hides the physical device from the process and applies the app's configuration. App 480's own
// configuration is Spacewar's action set, so the game saw no pad at all after Steam started. The game now
// gives Steam its own action manifest (one "gamepad" action set: every Xbox button, both sticks, both
// triggers), reads those actions through ISteamInput and hands them on as one ordinary Xbox pad, here.
// With Steam Input off for the pad, Steam reports no controller and SDL keeps reading the physical one.
//
// No Steam headers: the Steam worker publishes, the game's input driver and the launcher read.
#include <cstdint>
#include <filesystem>
#include <string>

namespace kk::steam_pad {
// XInput layout (buttons are XINPUT_GAMEPAD_* bits, thumbs positive right/up).
struct State {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  bool operator==(const State&) const = default;
};

// The manifest's digital actions, in this order; bit i of the pad is kDigitalBits[i].
constexpr int kDigitalCount = 14;
extern const char* const kDigitalNames[kDigitalCount];
extern const uint16_t kDigitalBits[kDigitalCount];
// Analog actions: left_stick, right_stick (joystick_move, -1..1, up positive), left_trigger, right_trigger (0..1).
constexpr int kAnalogCount = 4;
extern const char* const kAnalogNames[kAnalogCount];
constexpr const char* kActionSet = "kk_gamepad";

// One Steam controller's actions as an XInput pad, and two pads held at once as one (the stronger axis wins).
State FromActions(const bool digital[kDigitalCount], const float analog[kAnalogCount][2]);
void Merge(State& into, const State& pad);

// Writes the action manifest and its controller configurations into folder (kept between runs; rewritten
// when the text differs). Returns the manifest's absolute path, empty on failure.
std::filesystem::path WriteManifest(const std::filesystem::path& folder);
std::string ManifestText();
std::string BindingText(const char* controller_type);

// Worker side. controllers = what ISteamInput reports right now (0: Steam Input does not hold any pad).
void Publish(int controllers, const State& merged, double now);
void Stopped();
// Reader side: true while Steam Input holds at least one controller and the last sample is fresh.
bool Read(State& out, double now);
int Controllers();
// Rumble the game asked for (0..65535); the Steam worker forwards it to the controllers.
void SetVibration(uint16_t left, uint16_t right);
bool TakeVibration(uint16_t& left, uint16_t& right);
double Now();
}  // namespace kk::steam_pad
