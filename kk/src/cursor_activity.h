#pragma once

#include <array>
#include <cstdint>

namespace rex::ui { class ImGuiDrawer; class Window; }
namespace kk {
// Only new physical pad activity takes ownership. A held button must not hide
// the pointer again after the player has switched to the mouse.
struct CursorActivity {
  bool mouse = true;
  struct Pad {
    uint32_t buttons = 0;
    std::array<int, 6> axes{};
    bool operator==(const Pad&) const = default;
  };
  void Mouse() { mouse = true; }
  void Controller(const Pad& before, const Pad& now) {
    bool active = now.buttons != 0;
    for (int axis : now.axes) active |= axis != 0;
    if (active && now != before) mouse = false;
  }
};
void CreateCursorActivity(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window);
}  // namespace kk
