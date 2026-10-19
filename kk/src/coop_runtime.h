#pragma once

#include <cstdint>
#include <filesystem>

namespace rex::ui { class ImGuiDrawer; }
namespace kk::coop {
// App lifetime. Disabled returns before constructing any service, thread or socket.
void Start(const std::filesystem::path& user_data);
void Shutdown();
// Only caller: the existing per-frame game-thread hook in hfr_frame_time.cpp.
void GameFrame(uint8_t* base);
void CreateOverlay(rex::ui::ImGuiDrawer* drawer);
}  // namespace kk::coop
