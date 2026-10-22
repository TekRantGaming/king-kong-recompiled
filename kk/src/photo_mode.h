// Photo mode (task 208, owner decision 18 Oct; native-renderer/docs/photo-mode.md): opened from the pause
// menu. The game stays paused its own way while the 3D world keeps drawing; a free camera (freecam.cpp)
// flies around within a radius, Jack's first-person arms and weapon and the HUD are hidden, a photo grade
// and the picture settings apply (renderer, plugin/native_photo.h), and Take picture saves the game's
// frame (no UI) as a PNG. Nothing is saved and the player's settings never change.
#pragma once

#include <cstdint>
#include <filesystem>

namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
}  // namespace rex::ui

namespace kk::photo {

// Once a frame on the game thread (sub_8272CA20's hook, hfr_frame_time.cpp): ends photo mode when the
// pause under it ended.
void Frame(uint8_t* base);
bool Active();
// The pause menu may offer it now (the port's pause menu is open).
bool Available();
// From the pause menu (any thread).
void Enter();
// Back to the pause menu, or (resume_game) straight back to the game.
void Exit(bool resume_game);
// freecam.cpp, on each pad poll of the first player while photo mode is on: the controller's buttons for
// the panel (the sticks and triggers fly the camera).
void OnPad(uint16_t buttons, int16_t lx, int16_t ly, int16_t rx, int16_t ry, uint8_t lt, uint8_t rt);
// fov.cpp: the field of view (radians) the scene view gets while photo mode is on; `game_radians` is the
// game's own with the player's field of view setting (photo mode's default).
float SceneFieldOfView(float game_radians);
// Creates photo mode's panel for the rest of the session (the app, next to the pause menu).
void CreatePanel(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                 const std::filesystem::path& user_dir);

}  // namespace kk::photo
