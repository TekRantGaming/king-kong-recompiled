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
// F10: everything drawn over the game is hidden (screenshots).
bool HideHud();
// The game's own HUD is hidden: F10, or kk_hide_hud (saved; the Hardcore mod), in play.
bool HideGameHud();
// Called on every pad poll, including disconnects, before returning to the game.
void Pad(uint32_t user, uint8_t *state, bool connected);
#if defined(KK_DEV_TOOLS)
bool DevPadInput(uint32_t user, uint8_t *state);
// KK_DEV_SCRIPT's FREECAM press: switches the free camera on or off (F9), the script's sticks then fly it.
void DevToggle();
#endif
void Frame(uint8_t *base);
// Photo mode (photo_mode.cpp): the free camera inside it. PhotoStart turns it on from the game's view
// (the next frame's) and keeps it within `radius` metres of where it started (no streaming); F9 and F10
// do nothing meanwhile. Roll in radians, speed in metres a second; mouse_look: the mouse turns the camera
// (else the mouse is left to the photo panel). The controller: left stick moves, right stick looks,
// the triggers go down and up, the stick clicks go faster / slower; its buttons go to the panel
// (photo::OnPad). PhotoResetView takes the game's view again.
void PhotoStart(float radius);
void PhotoStop();
void PhotoParams(float roll, float speed, bool mouse_look);
void PhotoResetView();
bool PhotoActive();
// Save/override the game view before its normal camera/frustum setup; restore
// afterwards.
bool OverrideView(uint8_t *base, uint32_t view, uint32_t caller,
                  std::array<uint8_t, 68> &saved);
} // namespace kk::freecam
