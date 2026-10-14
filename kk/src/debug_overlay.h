#pragma once
#include <filesystem>
namespace rex::ui { class ImGuiDrawer; class Window; }
namespace kk {
void CreateDebugOverlay(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, const std::filesystem::path& config);
bool DebugOverlayOpen();
void SetDebugOverlayConfigPath(const std::filesystem::path& config);
}
