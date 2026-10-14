// The graphics panel (F8, debug_overlay.cpp): graphics settings tried live over the running game.
#pragma once
#include <filesystem>
namespace rex::ui { class ImGuiDrawer; class Window; }
namespace kk {
// Once, when the app makes its dialogs. `config`: the settings file Save writes; `user_root`: captures go
// to <user_root>/screenshots.
void CreateDebugOverlay(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, const std::filesystem::path& config,
                        const std::filesystem::path& user_root);
// The full panel is open: the mouse is the panel's (no mouse look, no clicks reach the game).
bool DebugOverlayOpen();
void SetDebugOverlayConfigPath(const std::filesystem::path& config);
}
