#include "ui_paint.h"

#include "overlay.h"

#include <chrono>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#if defined(KK_DEV_TOOLS)
#include <rex/logging.h>
namespace kk { double DevSecondsSinceMenu(); }
#endif

#include "frame_stats.h"
#include "platform.h"
#include "play_internal.h"
#include "settings.h"
#if defined(KK_NATIVE_RENDERER)
#include "hooks/kk_native_ui.h"  // the native renderer's pipeline stutter line
#endif

namespace kk {
namespace {
std::mutex paint_mutex;
std::function<void()> paint_request;
}
void SetUiPaintRequest(std::function<void()> request) {
  std::lock_guard lock(paint_mutex);
  paint_request = std::move(request);
}
void RequestUiPaint() {
  std::lock_guard lock(paint_mutex);
  if (paint_request) paint_request();
}
namespace {


class FpsOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit FpsOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

  bool NeedsContinuousPaint() override { return play::CoverScreen(); }

 protected:
  void OnDraw(ImGuiIO& io) override {
#if defined(KK_DEV_TOOLS)
    if (std::getenv("KK_DEV_UIPAINT_TEST")) {
      const double seconds = DevSecondsSinceMenu();
      static int phase = -1;
      const int next = seconds < 0 ? -1 : seconds < 12 ? 0 : seconds < 24 ? 1 : seconds < 36 ? 2 :
                       seconds < 48 ? 3 : seconds < 60 ? 4 : seconds < 72 ? 5 : 6;
      if (next >= 0 && next != phase) {
        phase = next;
        rex::cvar::SetFlagByName("ui_paint_on_demand", phase == 0 || phase == 2 || phase == 4 ? "false" : "true");
        rex::cvar::SetFlagByName("kk_frame_rate", phase < 2 || phase == 6 ? "30" : "0");
        rex::cvar::SetFlagByName("native_upscaler", phase == 4 || phase == 5 ? "dlss" : "off");
        rex::cvar::SetFlagByName("native_upscaler_quality", "quality");
        RequestUiPaint();
        REXLOG_INFO("UI repaint test phase {} at {:.3f} seconds", phase, seconds);
      }
    }
#endif
    // Black while the launcher's start request is carried out (the title and the game's menus never
    // show) and on the way back to the launcher.
    if (play::CoverScreen())
      ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 255));
    if (!REXCVAR_GET(kk_show_fps)) return;
    const auto stats = GetGuestFrameStats();
    if (stats.frame_count == 0) return;  // nothing to show yet (launcher, loading)
    char text[48];
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
#if defined(KK_NATIVE_RENDERER)
    // The native renderer's pipeline stutter counter (--native_stutter_stats), under the counter.
    const std::string stutter = nr::kk::StutterLine();
    if (!stutter.empty()) {
      const float line_size = 13.0f * s;  // not "small": a macro in the Windows headers
      const ImVec2 st = font->CalcTextSizeA(line_size, FLT_MAX, 0, stutter.c_str());
      const ImVec2 q1(io.DisplaySize.x - 14 * s, p1.y + 6 * s + st.y + pad.y * 2);
      const ImVec2 q0(q1.x - st.x - pad.x * 2, p1.y + 6 * s);
      dl->AddRectFilled(q0, q1, IM_COL32(7, 10, 22, 190), 8 * s);
      dl->AddText(font, line_size, ImVec2(q0.x + pad.x, q0.y + pad.y), IM_COL32(220, 220, 230, 255), stutter.c_str());
    }
#endif
  }
};

}  // namespace

#if defined(KK_DEV_TOOLS)
void CreatePauseStandIn(rex::ui::ImGuiDrawer* drawer);  // pause_hooks.cpp (KK_DEV_PAUSE_STANDIN)
#endif

void CreateFpsOverlay(rex::ui::ImGuiDrawer* drawer) {
  new FpsOverlay(drawer);  // lives for the rest of the session
#if defined(KK_DEV_TOOLS)
  CreatePauseStandIn(drawer);
#endif
  rex::ui::RegisterBind("bind_kk_fps", "F2", "Toggle frame counter", [] {
    rex::cvar::SetFlagByName("kk_show_fps", REXCVAR_GET(kk_show_fps) ? "false" : "true");
#if defined(KK_DEV_TOOLS)
    REXLOG_INFO("KK dev F2: {}", REXCVAR_GET(kk_show_fps) ? "open" : "closed");
#endif
    RequestUiPaint();
  });
}

}  // namespace kk
