#include "overlay.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>

#include "frame_stats.h"
#include "platform.h"
#include "settings.h"

namespace kk {
namespace {

#if defined(KK_DEV_TOOLS)
// Developer build: a short note after a test hotkey (F8/F9 ambient occlusion).
std::string g_note;
std::chrono::steady_clock::time_point g_note_until;

void ShowNote(std::string text) {
  g_note = std::move(text);
  g_note_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
}

void ShowAoNote() {
  static const char* const kModes[] = {"off", "on", "AO only"};
  const int mode = std::atoi(rex::cvar::GetFlagByName("ao_mode").c_str());
  ShowNote("Ambient occlusion: " + std::string(kModes[mode < 0 || mode > 2 ? 0 : mode]) + ", strength " +
           rex::cvar::GetFlagByName("ao_strength"));
}
#endif

class FpsOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit FpsOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
#if defined(KK_DEV_TOOLS)
    if (!g_note.empty() && std::chrono::steady_clock::now() < g_note_until) {
      const float s = ImGui::GetFontSize() / 18.0f;
      ImGui::GetForegroundDrawList()->AddText(nullptr, 22.0f * s, ImVec2(24 * s, 24 * s),
                                              IM_COL32(255, 230, 120, 255), g_note.c_str());
    }
#endif
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
  }
};

}  // namespace

void CreateFpsOverlay(rex::ui::ImGuiDrawer* drawer) {
  new FpsOverlay(drawer);  // lives for the rest of the session
  rex::ui::RegisterBind("bind_kk_fps", "F2", "Toggle frame counter", [] {
    rex::cvar::SetFlagByName("kk_show_fps", REXCVAR_GET(kk_show_fps) ? "false" : "true");
  });
#if defined(KK_DEV_TOOLS)
  // Ambient occlusion prototype (GPU plugin, REX_DEV_AO): F8 off / on / AO
  // only, F9 strength 1 / 2 / 3.
  rex::ui::RegisterBind("bind_kk_dev_ao", "F8", "Ambient occlusion (prototype)", [] {
    const int mode = std::atoi(rex::cvar::GetFlagByName("ao_mode").c_str());
    rex::cvar::SetFlagByName("ao_mode", std::to_string((mode + 1) % 3));
    ShowAoNote();
  });
  rex::ui::RegisterBind("bind_kk_dev_ao_strength", "F9", "Ambient occlusion strength (prototype)", [] {
    const int strength = std::atoi(rex::cvar::GetFlagByName("ao_strength").c_str());
    rex::cvar::SetFlagByName("ao_strength", std::to_string(strength % 3 + 1));
    ShowAoNote();
  });
#endif
}

}  // namespace kk
