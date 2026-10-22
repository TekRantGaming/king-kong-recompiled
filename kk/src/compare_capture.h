// Task 354: the owner's comparison capture (private; only with --kk_compare_capture=true). Two looks switchable
// live, Original (the Xbox 360 as shipped, its 720p picture stretched plainly) and Max (everything at its highest at
// the output resolution), a shot card from compare-shots.txt beside the exe, and a capture key that freezes the
// game and saves both looks of the same frame. Keys and choices: compare_capture.cpp.
#pragma once

#include <filesystem>
#include <functional>

namespace rex::ui {
class ImGuiDrawer;
class Window;
struct RawImage;
}  // namespace rex::ui

namespace kk::compare {

// --kk_compare_capture=true (never saved; nothing of this exists without it).
bool Enabled();
// Before the GPU starts (king_kong_app.h, before EnforceOriginalLook): the settings both looks need from the start
// (the Remastered look, a temporal upscaler so the frame is the output size), then the Max look. Never saved.
void ApplyAtGameStart(const std::filesystem::path& user_data);
// The card, the look label and the keys, for the rest of the session (only when Enabled).
void Create(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, const std::filesystem::path& user_dir);
// The app's guest output reader (the presenter's CaptureGuestOutput; any thread).
void SetGrabber(std::function<bool(rex::ui::RawImage&)> grab);
// pause_hooks.cpp FilterPad: true while the capture key is pressing Start for the player (to pause the game).
bool PressStart();

}  // namespace kk::compare
