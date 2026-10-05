// In-game overlays for the port (frame counter).

#pragma once

namespace rex::ui {
class ImGuiDrawer;
}

namespace kk {

// Creates the frame-rate counter (shown when kk_show_fps is on; F2 toggles).
void CreateFpsOverlay(rex::ui::ImGuiDrawer* drawer);

}  // namespace kk
