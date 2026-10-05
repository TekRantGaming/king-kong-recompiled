#include "overlay.h"

#include <cstdio>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include "frame_stats.h"
#include "platform.h"
#include "settings.h"

namespace kk {
namespace {

class FpsOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit FpsOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!REXCVAR_GET(kk_show_fps)) return;
    const auto stats = GetGuestFrameStats();
    char text[48];
    if (stats.frame_count == 0)
      std::snprintf(text, sizeof(text), "-- FPS");
    else
      std::snprintf(text, sizeof(text), "%.0f FPS  %.1f ms", stats.fps, stats.frame_time_ms);
    const float s = ImGui::GetFontSize() / 18.0f;
    ImFont* font = GetUiFonts().semibold ? GetUiFonts().semibold : ImGui::GetFont();
    const float size = 16.0f * s;
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, text);
    const ImVec2 pad(10 * s, 5 * s);
    const ImVec2 p1(io.DisplaySize.x - 14 * s, 14 * s + ts.y + pad.y * 2);
    const ImVec2 p0(p1.x - ts.x - pad.x * 2, 14 * s);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(7, 10, 22, 190), 8 * s);
    const ImU32 color = stats.fps >= 55 ? IM_COL32(139, 213, 80, 255)
                        : stats.fps >= 28 ? IM_COL32(240, 200, 90, 255)
                                          : IM_COL32(240, 110, 90, 255);
    dl->AddText(font, size, ImVec2(p0.x + pad.x, p0.y + pad.y), color, text);
  }
};

}  // namespace

void CreateFpsOverlay(rex::ui::ImGuiDrawer* drawer) {
  new FpsOverlay(drawer);  // lives for the rest of the session
  rex::ui::RegisterBind("bind_kk_fps", "F2", "Toggle frame counter", [] {
    rex::cvar::SetFlagByName("kk_show_fps", REXCVAR_GET(kk_show_fps) ? "false" : "true");
  });
}

}  // namespace kk
