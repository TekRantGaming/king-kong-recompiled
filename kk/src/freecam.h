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
// Called on every pad poll, including disconnects, before returning to the game.
void Pad(uint32_t user, uint8_t *state, bool connected);
#if defined(KK_DEV_TOOLS)
bool DevPadInput(uint32_t user, uint8_t *state);
// KK_DEV_SCRIPT's FREECAM press: switches the free camera on or off (F9), the script's sticks then fly it.
void DevToggle();
#endif
void Frame(uint8_t *base);
// Save/override the game view before its normal camera/frustum setup; restore
// afterwards.
bool OverrideView(uint8_t *base, uint32_t view, uint32_t caller,
                  std::array<uint8_t, 68> &saved);
} // namespace kk::freecam
