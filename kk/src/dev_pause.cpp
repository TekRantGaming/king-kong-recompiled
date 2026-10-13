// Developer-only: pause menu investigation (never shipped). Findings:
// native-renderer/docs/chapter-jump.md, "Replacing the pause menu".
//
// KK_DEV_PAGE_TRACE=1: log when each menu page script (the IntMIG_* functions
// of the AI function table at 0x828CB140, one runs every frame while its page
// is up) starts running, and every page switch request (sub_826FA3B0).
// (IntMIG_Page_StartMain, IntMIG_loop and IntMIG_init are hooked elsewhere:
// cheats.cpp, fov.cpp, dev_chapter_jump.cpp.)
#if defined(KK_DEV_TOOLS)

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <string>

#include <imgui.h>
#include <rex/ui/imgui_dialog.h>

#include <rex/hook.h>
#include <rex/logging.h>

namespace kk::pause {
namespace {
bool Tracing() {
  static const bool on = std::getenv("KK_DEV_PAGE_TRACE") != nullptr;
  return on;
}
uint32_t Load32(const uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void PageRuns(const char* name, uint8_t* base) {
  static std::mutex m;
  static std::unordered_map<const char*, std::chrono::steady_clock::time_point> last;
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(m);
  auto& t = last[name];
  if (now - t > std::chrono::milliseconds(400)) {
    const uint32_t ai = Load32(base, 0x82CCBF64), v = ai ? Load32(base, ai + 0x40) : 0;
    REXLOG_INFO("KK dev page: {} runs (vars {:08X}, item {})", name, v, v ? int32_t(Load32(base, v + 0x3C)) : -1);
  }
  t = now;
}
}  // namespace
}  // namespace kk::pause

#define KK_PAGE_HOOK(addr, name)                                REX_EXTERN(__imp__sub_##addr);                                REX_HOOK_RAW(sub_##addr) {                                      if (kk::pause::Tracing()) kk::pause::PageRuns(name, base);     __imp__sub_##addr(ctx, base);                               }

KK_PAGE_HOOK(8249ED88, "Interface_MenuInGame")
KK_PAGE_HOOK(825ECC98, "interface_MenuInGame_tools")
KK_PAGE_HOOK(824C5050, "IntMIG_Page_DemoXenon_Extras")
KK_PAGE_HOOK(824C4948, "IntMIG_Page_DemoXenon_Main")
KK_PAGE_HOOK(824C46D8, "IntMIG_Page_Xbox360_save_game_with_no_device")
KK_PAGE_HOOK(824C3F90, "IntMIG_Page_Xbox360_Invalid_Storage_Device")
KK_PAGE_HOOK(824C3CB0, "IntMIG_Page_Xbox360_Controller_Check")
KK_PAGE_HOOK(824C39B0, "IntMIG_Xbox360_Profile_Loaded_Before_Gamplay")
KK_PAGE_HOOK(824C3618, "IntMIG_Page_Xbox360_Profile_Changed_During_StartMenu")
KK_PAGE_HOOK(824C30C8, "IntMIG_Page_Xbox360_Profile_Changed_During_Gameplay")
KK_PAGE_HOOK(824D2F20, "IntMIG_Page_OptStart_Langue")
KK_PAGE_HOOK(824D1958, "IntMIG_Page_ScoreNew")
KK_PAGE_HOOK(824C1A48, "IntMIG_Page_CheatFinal")
KK_PAGE_HOOK(824C17F8, "IntMIG_Page_Memcard_TestError")
KK_PAGE_HOOK(824CF400, "IntMIG_Page_OptStart_Control")
KK_PAGE_HOOK(824CE4A0, "IntMIG_Page_OptStart_Video")
KK_PAGE_HOOK(824CD320, "IntMIG_Page_OptStart_Audio")
KK_PAGE_HOOK(824C0F60, "IntMIG_Page_OptStart")
KK_PAGE_HOOK(824A0E70, "IntMIG_Background")
KK_PAGE_HOOK(824AC7E8, "IntMIG_afairetoutletemps")
KK_PAGE_HOOK(824C0A30, "IntMIG_Page_E3Menu")
KK_PAGE_HOOK(8266F908, "interface_MenuIGame_memcard")
KK_PAGE_HOOK(824C02B0, "IntMIG_Page_ProfileDelete")
KK_PAGE_HOOK(824BF288, "IntMIG_Page_ProfileCreate")
KK_PAGE_HOOK(824BDC28, "IntMIG_Page_Profile")
KK_PAGE_HOOK(824CBFE8, "IntMIG_Page_Xtra")
KK_PAGE_HOOK(824BD150, "IntMIG_Page_Map")
KK_PAGE_HOOK(824B4DF0, "IntMIG_Page_load")
KK_PAGE_HOOK(824CB738, "IntMIG_Page_Memcard")
KK_PAGE_HOOK(8249CE68, "IntMIG_Page_Cheat")
KK_PAGE_HOOK(8249EBD8, "IntMIG_colorize")
KK_PAGE_HOOK(824B44E8, "IntMIG_wait")
KK_PAGE_HOOK(824BA188, "IntMIG_Page_Xbox360_NotEnoughSpace")
KK_PAGE_HOOK(824B9F50, "IntMIG_Page_Xbox360_Quit_Demo")

// Page switch requests: sub_826FA3B0(mode, page) from a page script.
REX_EXTERN(__imp__sub_826FA3B0);
REX_HOOK_RAW(sub_826FA3B0) {
  if (kk::pause::Tracing())
    REXLOG_INFO("KK dev page: switch r3={} r4={:08X} lr={:08X}", int32_t(ctx.r3.u32), ctx.r4.u32, uint32_t(ctx.lr));
  __imp__sub_826FA3B0(ctx, base);
}

// KK_DEV_PAUSE_OVERLAY=1: a stand-in for the game's pause menu, drawn by the
// port. The game still pauses its own way (Start in play: IntMIG_wait,
// sub_824B44E8, plays the pause sound, notes the time and opens
// IntMIG_Page_Main, sub_824BA560, the pause page). While that page is up in
// play, the port does not run it (so it draws nothing and reads nothing),
// eats the pad (the game sees no buttons) and shows its own list. Choosing an
// item runs the game's page for a frame with that item selected and A pressed
// (the button query sub_8272B7E8 answers for it), and the Yes of the
// confirmation page (IntMIG_Page_MessageYesNo, sub_824BC1B0) the same way:
// the game's own code does the rest. Item ids in the pause page: 6 Resume,
// 8 Start again, 3 Quit game, 5 Options, 7 Score.
namespace kk {
extern void (*g_dev_pad_filter)(uint32_t user, uint8_t* state);
}

namespace kk::pause {
namespace {
bool OverlayOn() {
  static const bool on = std::getenv("KK_DEV_PAUSE_OVERLAY") != nullptr;
  return on;
}
void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  uint8_t* p = base + addr;
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

enum class Action { kNone, kResume, kRestart, kQuit };
std::atomic<int64_t> g_main_seen_ms{-100000};  // when the pause page last ran in play
std::atomic<bool> g_has_restart{false};        // a save is loaded: the game offers Start again
std::atomic<int> g_sel{0};
std::atomic<int> g_pending{0};                 // Action
int g_force_button = -1;                       // the game's button query answers "pressed" for this one
int g_try_index = 0, g_phase = 0;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
bool OursUp() { return OverlayOn() && NowMs() - g_main_seen_ms.load() < 250; }

struct Item {
  const char* label;
  Action action;
};
std::vector<Item> Items() {
  std::vector<Item> items{{"Resume", Action::kResume}};
  if (g_has_restart) items.push_back({"Start again", Action::kRestart});
  items.push_back({"Quit game", Action::kQuit});
  return items;
}

const char* ActionName(int a) {
  static const char* const kNames[] = {"none", "Resume", "Start again", "Quit game"};
  return kNames[a];
}

// The pad, while our list is up: UP/DOWN move, A chooses, B or START resume.
// The game is given an empty pad.
void PadFilter(uint32_t user, uint8_t* state) {
  if (user != 0 || !OursUp()) return;
  const uint16_t buttons = uint16_t(state[4] << 8 | state[5]);
  static uint16_t last = 0;
  const uint16_t pressed = buttons & ~last;
  last = buttons;
  const auto items = Items();
  if (g_pending == int(Action::kNone)) {
    int sel = std::min<int>(g_sel, int(items.size()) - 1);
    if (pressed & 0x0001) sel = (sel + int(items.size()) - 1) % int(items.size());
    if (pressed & 0x0002) sel = (sel + 1) % int(items.size());
    if (sel != g_sel) {
      g_sel = sel;
      REXLOG_INFO("KK dev pause: our list: {} selected", items[size_t(sel)].label);
    }
    Action chosen = Action::kNone;
    if (pressed & 0x1000) chosen = items[size_t(sel)].action;
    if (pressed & (0x2000 | 0x0010)) chosen = Action::kResume;
    if (chosen != Action::kNone) {
      REXLOG_INFO("KK dev pause: our list: {} chosen", ActionName(int(chosen)));
      g_try_index = 0;
      g_phase = 0;
      g_pending = int(chosen);
    }
  }
  std::memset(state + 4, 0, 12);  // the game sees no buttons, triggers or sticks
}

int WantedId(int action) { return action == int(Action::kResume) ? 6 : action == int(Action::kRestart) ? 8 : 3; }

class PauseOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit PauseOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const bool up = OursUp();
    static bool was_up = false;
    if (up != was_up) REXLOG_INFO("KK dev pause: our list {}", up ? "drawn" : "gone");
    was_up = up;
    if (!up) return;
    const auto items = Items();
    const float s = ImGui::GetFontSize() / 18.0f;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(5, 8, 18, 170));
    const ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    const float row = 44 * s, w = 360 * s, h = row * float(items.size()) + 90 * s;
    const ImVec2 p0(c.x - w / 2, c.y - h / 2), p1(c.x + w / 2, c.y + h / 2);
    dl->AddRectFilled(p0, p1, IM_COL32(16, 22, 40, 235), 14 * s);
    dl->AddText(nullptr, 26 * s, ImVec2(p0.x + 28 * s, p0.y + 20 * s), IM_COL32(235, 235, 245, 255), "Paused (port)");
    for (size_t i = 0; i < items.size(); ++i) {
      const ImVec2 r0(p0.x + 20 * s, p0.y + 70 * s + row * float(i)), r1(p1.x - 20 * s, r0.y + row - 6 * s);
      const bool sel = int(i) == g_sel;
      if (sel) dl->AddRectFilled(r0, r1, IM_COL32(70, 110, 200, 230), 8 * s);
      dl->AddText(nullptr, 22 * s, ImVec2(r0.x + 16 * s, r0.y + 9 * s),
                  sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(190, 195, 210, 255), items[i].label);
    }
  }
};

const bool g_installed = [] {
  if (OverlayOn()) kk::g_dev_pad_filter = PadFilter;
  return true;
}();
}  // namespace
}  // namespace kk::pause

namespace kk {
void CreateDevPauseOverlay(rex::ui::ImGuiDrawer* drawer) {
  if (pause::OverlayOn()) new pause::PauseOverlay(drawer);
}
}  // namespace kk

// The game's button query (pressed this frame): button r3 of the active pad.
REX_EXTERN(__imp__sub_8272B7E8);
REX_HOOK_RAW(sub_8272B7E8) {
  if (kk::pause::g_force_button >= 0) {
    ctx.r3.u64 = int(ctx.r3.u32) == kk::pause::g_force_button ? 1 : 0;
    return;
  }
  __imp__sub_8272B7E8(ctx, base);
}

// The pause page (IntMIG_Page_Main; also the start menu's main page).
REX_EXTERN(__imp__sub_824BA560);
REX_HOOK_RAW(sub_824BA560) {
  using namespace kk::pause;
  if (Tracing()) PageRuns("IntMIG_Page_Main", base);
  const uint32_t ai = Load32(base, 0x82CCBF64), v = ai ? Load32(base, ai + 0x40) : 0;
  const uint32_t g = Load32(base, 0x82CC97C0);
  const bool in_play = g && int32_t(Load32(base, g + 8)) != 3;  // not the start menu's level
  if (!OverlayOn() || !in_play || !v) {
    __imp__sub_824BA560(ctx, base);
    return;
  }
  if (NowMs() - g_main_seen_ms.load() > 1000) {  // the pause page just opened
    g_sel = 0;
    g_pending = int(Action::kNone);
    g_phase = 0;
    REXLOG_INFO("KK dev pause: the game paused; its page is hidden");
  }
  g_main_seen_ms = NowMs();
  g_has_restart = int32_t(Load32(base, g + 0x4E10)) != -1;
  const int pending = g_pending;
  if (pending == int(Action::kNone) || g_phase == 2) return;  // hidden: the game's page does not run
  // Find the row of the wanted item (the page keeps the selected item's id
  // at v+0x44), then run the page once more with A pressed.
  const int want = WantedId(pending);
  if (g_phase == 0) {
    Store32(base, v + 0x3C, uint32_t(g_try_index));
    g_force_button = 99;  // nothing pressed
    __imp__sub_824BA560(ctx, base);
    g_force_button = -1;
    if (int32_t(Load32(base, v + 0x44)) == want) {
      g_phase = 1;
    } else if (++g_try_index > 8) {
      REXLOG_WARN("KK dev pause: the pause page has no item {}", want);
      g_pending = int(Action::kNone);
    }
    return;
  }
  Store32(base, v + 0x3C, uint32_t(g_try_index));
  g_force_button = 0;  // A
  __imp__sub_824BA560(ctx, base);
  g_force_button = -1;
  REXLOG_INFO("KK dev pause: ran the game's pause page with item {} (row {}) and A pressed", want, g_try_index);
  if (pending == int(Action::kResume)) {
    g_pending = int(Action::kNone);
    g_phase = 0;
    g_main_seen_ms = -100000;
  } else {
    g_phase = 2;  // the game's confirmation page comes next
  }
}

// The confirmation page (IntMIG_Page_MessageYesNo): Yes for our choice.
REX_EXTERN(__imp__sub_824BC1B0);
REX_HOOK_RAW(sub_824BC1B0) {
  using namespace kk::pause;
  if (Tracing()) PageRuns("IntMIG_Page_MessageYesNo", base);
  const uint32_t g = Load32(base, 0x82CC97C0);
  const bool in_play = g && int32_t(Load32(base, g + 8)) != 3;
  const int pending = g_pending;
  if (!OverlayOn() || !in_play || g_phase != 2 ||
      (pending != int(Action::kRestart) && pending != int(Action::kQuit))) {
    __imp__sub_824BC1B0(ctx, base);
    return;
  }
  g_main_seen_ms = NowMs();
  static int frames = 0;
  if (++frames < 3) {  // let it set itself up first (hidden: no A yet)
    g_force_button = 99;
    __imp__sub_824BC1B0(ctx, base);
    g_force_button = -1;
    return;
  }
  frames = 0;
  g_force_button = 0;  // A on its default (Yes)
  __imp__sub_824BC1B0(ctx, base);
  g_force_button = -1;
  REXLOG_INFO("KK dev pause: answered Yes on the game's confirmation page ({})", ActionName(pending));
  g_pending = int(Action::kNone);
  g_phase = 0;
  g_main_seen_ms = -100000;
}

#endif
