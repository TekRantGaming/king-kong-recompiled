#include "steam_pad.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <system_error>

namespace kk::steam_pad {
const char* const kDigitalNames[kDigitalCount] = {
    "pad_a", "pad_b", "pad_x", "pad_y", "pad_lb", "pad_rb", "pad_back", "pad_start",
    "pad_ls", "pad_rs", "pad_up", "pad_down", "pad_left", "pad_right"};
const uint16_t kDigitalBits[kDigitalCount] = {
    0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0x0020, 0x0010,
    0x0040, 0x0080, 0x0001, 0x0002, 0x0004, 0x0008};
const char* const kAnalogNames[kAnalogCount] = {"pad_left_stick", "pad_right_stick", "pad_lt", "pad_rt"};

namespace {
const char* const kTitles[kDigitalCount] = {"A", "B", "X", "Y", "Left bumper", "Right bumper", "Back", "Start",
                                            "Left stick click", "Right stick click", "Dpad up", "Dpad down",
                                            "Dpad left", "Dpad right"};
const char* const kAnalogTitles[kAnalogCount] = {"Left stick", "Right stick", "Left trigger", "Right trigger"};
// Steam's configuration types; every one gets the same layout (Steam names the sources the same way).
const char* const kControllerTypes[] = {"controller_xboxone", "controller_xbox360", "controller_xboxelite",
                                        "controller_ps4", "controller_ps5", "controller_ps5_edge",
                                        "controller_switch_pro", "controller_generic"};

int16_t Axis(float v) {
  if (!std::isfinite(v)) return 0;
  v = std::clamp(v, -1.0f, 1.0f);
  return int16_t(std::lround(v * 32767.0f));
}
uint8_t Trigger(float v) {
  if (!std::isfinite(v)) return 0;
  return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}
int16_t Stronger(int16_t a, int16_t b) { return std::abs(int(b)) > std::abs(int(a)) ? b : a; }

std::mutex mutex;
State sample;
int controllers = 0;
double sampled_at = -1e9;
uint16_t rumble_left = 0, rumble_right = 0;
bool rumble_changed = false;

std::string ActionsBlock() {
  std::ostringstream o;
  o << "\t\"" << kActionSet << "\"\n\t{\n\t\t\"title\"\t\t\"#set_gamepad\"\n\t\t\"legacy_set\"\t\t\"0\"\n";
  o << "\t\t\"StickPadGyro\"\n\t\t{\n";
  for (int i = 0; i < 2; ++i)
    o << "\t\t\t\"" << kAnalogNames[i] << "\"\n\t\t\t{\n\t\t\t\t\"title\"\t\t\"#" << kAnalogNames[i]
      << "\"\n\t\t\t\t\"input_mode\"\t\t\"joystick_move\"\n\t\t\t}\n";
  o << "\t\t}\n\t\t\"AnalogTrigger\"\n\t\t{\n";
  for (int i = 2; i < kAnalogCount; ++i) o << "\t\t\t\"" << kAnalogNames[i] << "\"\t\t\"#" << kAnalogNames[i] << "\"\n";
  o << "\t\t}\n\t\t\"Button\"\n\t\t{\n";
  for (int i = 0; i < kDigitalCount; ++i) o << "\t\t\t\"" << kDigitalNames[i] << "\"\t\t\"#" << kDigitalNames[i] << "\"\n";
  o << "\t\t}\n\t}\n";
  return o.str();
}
std::string Localization() {
  std::ostringstream o;
  o << "\"localization\"\n{\n\t\"english\"\n\t{\n\t\t\"title\"\t\t\"King Kong controller\"\n"
    << "\t\t\"description\"\t\t\"Every button, stick and trigger as an Xbox controller.\"\n"
    << "\t\t\"set_gamepad\"\t\t\"Controller\"\n";
  for (int i = 0; i < kAnalogCount; ++i) o << "\t\t\"" << kAnalogNames[i] << "\"\t\t\"" << kAnalogTitles[i] << "\"\n";
  for (int i = 0; i < kDigitalCount; ++i) o << "\t\t\"" << kDigitalNames[i] << "\"\t\t\"" << kTitles[i] << "\"\n";
  o << "\t}\n}\n";
  return o.str();
}
std::string Press(const char* input, const char* action) {
  std::ostringstream o;
  o << "\t\t\t\"" << input << "\"\n\t\t\t{\n\t\t\t\t\"activators\"\n\t\t\t\t{\n\t\t\t\t\t\"Full_Press\"\n\t\t\t\t\t{\n"
    << "\t\t\t\t\t\t\"bindings\"\n\t\t\t\t\t\t{\n\t\t\t\t\t\t\t\"binding\"\t\t\"game_action " << kActionSet << " " << action
    << "\"\n\t\t\t\t\t\t}\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n";
  return o.str();
}
std::string Group(int id, const char* mode, const std::string& inputs, const char* analog = nullptr) {
  std::ostringstream o;
  o << "\"group\"\n{\n\t\"id\"\t\t\"" << id << "\"\n\t\"mode\"\t\t\"" << mode << "\"\n\t\"inputs\"\n\t{\n" << inputs << "\t}\n";
  if (analog) o << "\t\"gameactions\"\n\t{\n\t\t\"" << kActionSet << "\"\t\t\"" << analog << "\"\n\t}\n";
  o << "}\n";
  return o.str();
}
bool SameFile(const std::filesystem::path& file, const std::string& text) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  const std::string old((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return old == text;
}
bool WriteText(const std::filesystem::path& file, const std::string& text) {
  if (SameFile(file, text)) return true;
  const auto temp = std::filesystem::path(file).concat(".tmp");
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    if (!out.flush()) return false;
  }
  std::error_code ec;
  std::filesystem::rename(temp, file, ec);
  return !ec;
}
std::string FileFor(const char* type) { return std::string("kk_input_") + type + ".vdf"; }
}  // namespace

State FromActions(const bool digital[kDigitalCount], const float analog[kAnalogCount][2]) {
  State s;
  for (int i = 0; i < kDigitalCount; ++i)
    if (digital[i]) s.buttons |= kDigitalBits[i];
  s.lx = Axis(analog[0][0]); s.ly = Axis(analog[0][1]);
  s.rx = Axis(analog[1][0]); s.ry = Axis(analog[1][1]);
  s.lt = Trigger(analog[2][0]); s.rt = Trigger(analog[3][0]);
  return s;
}
void Merge(State& into, const State& pad) {
  into.buttons |= pad.buttons;
  into.lt = std::max(into.lt, pad.lt); into.rt = std::max(into.rt, pad.rt);
  into.lx = Stronger(into.lx, pad.lx); into.ly = Stronger(into.ly, pad.ly);
  into.rx = Stronger(into.rx, pad.rx); into.ry = Stronger(into.ry, pad.ry);
}

std::string ManifestText() {
  std::ostringstream o;
  o << "\"Action Manifest\"\n{\n\t\"configurations\"\n\t{\n";
  for (const char* type : kControllerTypes)
    o << "\t\t\"" << type << "\"\n\t\t{\n\t\t\t\"0\"\n\t\t\t{\n\t\t\t\t\"path\"\t\t\"" << FileFor(type) << "\"\n\t\t\t}\n\t\t}\n";
  o << "\t}\n\t\"actions\"\n\t{\n" << ActionsBlock() << "\t}\n" << Localization() << "}\n";
  return o.str();
}

std::string BindingText(const char* controller_type) {
  std::ostringstream o;
  o << "\"controller_mappings\"\n{\n\t\"version\"\t\t\"3\"\n\t\"revision\"\t\t\"1\"\n\t\"title\"\t\t\"#title\"\n"
    << "\t\"description\"\t\t\"#description\"\n\t\"controller_type\"\t\t\"" << controller_type << "\"\n"
    << "\t\"major_revision\"\t\t\"0\"\n\t\"minor_revision\"\t\t\"0\"\n";
  o << "\"actions\"\n{\n" << ActionsBlock() << "}\n" << Localization();
  o << Group(0, "four_buttons", Press("button_a", "pad_a") + Press("button_b", "pad_b") + Press("button_x", "pad_x") +
                                    Press("button_y", "pad_y"));
  o << Group(1, "dpad", Press("dpad_north", "pad_up") + Press("dpad_south", "pad_down") + Press("dpad_west", "pad_left") +
                            Press("dpad_east", "pad_right"));
  o << Group(2, "joystick_move", Press("click", "pad_ls"), "pad_left_stick");
  o << Group(3, "joystick_move", Press("click", "pad_rs"), "pad_right_stick");
  o << Group(4, "trigger", "", "pad_lt");
  o << Group(5, "trigger", "", "pad_rt");
  o << Group(6, "switches", Press("button_escape", "pad_start") + Press("button_menu", "pad_back") +
                                Press("left_bumper", "pad_lb") + Press("right_bumper", "pad_rb"));
  o << "\"preset\"\n{\n\t\"id\"\t\t\"0\"\n\t\"name\"\t\t\"" << kActionSet << "\"\n\t\"group_source_bindings\"\n\t{\n"
    << "\t\t\"0\"\t\t\"button_diamond active\"\n\t\t\"1\"\t\t\"dpad active\"\n\t\t\"2\"\t\t\"joystick active\"\n"
    << "\t\t\"3\"\t\t\"right_joystick active\"\n\t\t\"4\"\t\t\"left_trigger active\"\n\t\t\"5\"\t\t\"right_trigger active\"\n"
    << "\t\t\"6\"\t\t\"switch active\"\n\t}\n}\n";
  o << "\"settings\"\n{\n}\n}\n";
  return o.str();
}

std::filesystem::path WriteManifest(const std::filesystem::path& folder) {
  if (folder.empty()) return {};
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const auto absolute = std::filesystem::absolute(folder, ec);
  if (ec) return {};
  for (const char* type : kControllerTypes)
    if (!WriteText(absolute / FileFor(type), BindingText(type))) return {};
  const auto manifest = absolute / "kk_input_manifest.vdf";
  if (!WriteText(manifest, ManifestText())) return {};
  return manifest;
}

double Now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void Publish(int count, const State& merged, double now) {
  std::lock_guard lock(mutex);
  controllers = std::max(0, count);
  sample = controllers ? merged : State{};
  sampled_at = now;
}
void Stopped() {
  std::lock_guard lock(mutex);
  controllers = 0; sample = {}; sampled_at = -1e9;
}
bool Read(State& out, double now) {
  std::lock_guard lock(mutex);
  // A stalled worker (Steam gone) must not leave a button held.
  if (!controllers || now - sampled_at > 0.25) return false;
  out = sample;
  return true;
}
int Controllers() {
  std::lock_guard lock(mutex);
  return controllers;
}
void SetVibration(uint16_t left, uint16_t right) {
  std::lock_guard lock(mutex);
  if (left == rumble_left && right == rumble_right) return;
  rumble_left = left; rumble_right = right; rumble_changed = true;
}
bool TakeVibration(uint16_t& left, uint16_t& right) {
  std::lock_guard lock(mutex);
  if (!rumble_changed) return false;
  rumble_changed = false; left = rumble_left; right = rumble_right;
  return true;
}
}  // namespace kk::steam_pad
