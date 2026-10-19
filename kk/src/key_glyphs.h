// Keyboard and mouse pictures (glyphs/keyboard/<name>.png) for a bound key (Beta 12, owner 19 Oct 2026).
//
// The keyboard set names most pictures after the key (W.png, Space.png), but the mouse buttons are bound as
// LMB / RMB / MMB (keybind_* cvars) and drawn as Mouse_Left / Mouse_Right / Mouse_Middle; turning the camera with
// the mouse (look and aim) is Mouse_Simple. Used by the in-game button prompts (glyphs.cpp), the Controls page's
// key rows (launcher.cpp, the same page in the pause menu), the setup's prompt preview and photo mode's hint.
// No game or ImGui needed (tests/launcher_logic_tests.cpp).

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace kk::key_glyphs {

// Mouse movement: the camera with Mouse camera on (mnk_mouse).
inline constexpr const char* kMouseMove = "Mouse_Simple";
// The right mouse button (photo mode looks around while it is held).
inline constexpr const char* kMouseRight = "Mouse_Right";

// The picture name for one bound key ("LMB", "Ctrl+E", " Space "): the key without modifiers or spaces, with the
// mouse buttons renamed. Empty for an empty key.
inline std::string PictureName(std::string_view key) {
  if (const size_t plus = key.rfind('+'); plus != std::string_view::npos && plus + 1 < key.size())
    key = key.substr(plus + 1);
  while (!key.empty() && key.front() == ' ') key.remove_prefix(1);
  while (!key.empty() && key.back() == ' ') key.remove_suffix(1);
  if (key == "LMB") return "Mouse_Left";
  if (key == "RMB") return "Mouse_Right";
  if (key == "MMB") return "Mouse_Middle";
  return std::string(key);
}

// Every key of a binding ("LMB,E") as picture names, in order.
inline std::vector<std::string> PictureNames(std::string_view binds) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= binds.size()) {
    size_t end = binds.find(',', start);
    if (end == std::string_view::npos) end = binds.size();
    std::string name = PictureName(binds.substr(start, end - start));
    if (!name.empty()) out.push_back(std::move(name));
    start = end + 1;
  }
  return out;
}

}  // namespace kk::key_glyphs
