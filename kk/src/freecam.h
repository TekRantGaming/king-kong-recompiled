#pragma once
#include <array>
#include <cstdint>
namespace rex::ui {
class ImGuiDrawer;
class Window;
} // namespace rex::ui
namespace kk::freecam {
void Install(rex::ui::ImGuiDrawer *, rex::ui::Window *);
bool Active();
bool HideHud();
void Frame(uint8_t *base);
// Save/override the game view before its normal camera/frustum setup; restore
// afterwards.
bool OverrideView(uint8_t *base, uint32_t view, uint32_t caller,
                  std::array<uint8_t, 68> &saved);
} // namespace kk::freecam
