// The graphics panel (F8, debug_overlay.cpp): graphics settings tried live over the running game.
#pragma once
#include <filesystem>
#include <cstdint>
namespace rex::ui { class ImGuiDrawer; class Window; }
namespace kk {
// Once, when the app makes its dialogs. `config`: the settings file Save writes; `user_root`: captures go
// to <user_root>/screenshots.
void CreateDebugOverlay(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, const std::filesystem::path& config,
                        const std::filesystem::path& user_root);
// The full panel is open: mouse and controller input belong to the panel.
bool DebugOverlayOpen();
// Before remapping, on the game thread. Returns true when the panel consumed
// this sample, including the opening chord and held controls after closing.
bool DebugOverlayFilterPad(uint32_t user, uint8_t* state);
void SetDebugOverlayConfigPath(const std::filesystem::path& config);
}
