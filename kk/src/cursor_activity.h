#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>

namespace rex::ui { class ImGuiDrawer; class Window; }
namespace kk {
// Only new physical pad activity takes ownership: a button pressed, or an axis
// that jumped. A held button, or a pad whose stick or trigger jitters past its
// deadzone, must not hide the pointer while the player uses the mouse.
struct CursorActivity {
  bool mouse = true;
  struct Pad {
    uint32_t buttons = 0;
    std::array<int, 6> axes{};
    bool operator==(const Pad&) const = default;
  };
  void Mouse() { mouse = true; }
  static constexpr int kAxisJump = 3;  // in Pad::axes units, 2048 of SDL's range
  void Controller(const Pad& before, const Pad& now) {
    bool active = (now.buttons & ~before.buttons) != 0;
    for (size_t i = 0; i < now.axes.size(); ++i) {
      active |= std::abs(now.axes[i]) - std::abs(before.axes[i]) >= kAxisJump;  // pushed further, not let go
    }
    if (active) mouse = false;
  }
};
void CreateCursorActivity(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window);
}  // namespace kk
