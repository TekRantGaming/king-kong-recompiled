// In-game overlays for the port (Performance metrics).

#pragma once

namespace rex::ui {
class ImGuiDrawer;
}

namespace kk {

// Creates the Performance metrics overlay (kk_performance_metrics: off, fps or full; F2 switches).
void CreateFpsOverlay(rex::ui::ImGuiDrawer* drawer);

}  // namespace kk
