// The pause takeover (native-renderer/docs/menus.md, contract play_session.h; research:
// native-renderer/docs/chapter-jump.md, "Replacing the pause menu").
//
// The game still pauses its own way: Start in play makes IntMIG_wait (sub_824B44E8) play the pause sound,
// note the time and open its pause page IntMIG_Page_Main (sub_824BA560), and the world stands still. From
// then on:
//   - the port does not run that page (it draws nothing and reads nothing) and its pause menu is open
//     (PauseMenuOpen) instead;
//   - the game draws a black screen while paused: the last frame of play, captured as the pause starts,
//     is drawn behind the port's menu instead (CreatePauseBackdrop);
//   - the game reads an empty pad (FilterPad, input_remap.cpp);
//   - an item the player chooses runs the game's page for a frame with that item's row selected and A
//     answered by the button query (sub_8272B7E8), and then the Yes of the game's confirmation page
//     (IntMIG_Page_MessageYesNo, sub_824BC1B0) the same way: the game's own code does the rest.
// Start in a cutscene still skips it, and loading screens never open the pause page: both are the game's.
// The Score item shows the game's own score page (drawn and read by the game) and comes back to the menu.
//
// The pause page's items (its list is built every frame: +0x3C the selected row, +0x44 the selected item's
// id): 2 Options, 3 Quit game, 8 Start again, 4 Resume, 0 Score; Start again and Score only in a
// chapter-select run (G+0x5668 != 0). With a save slot outside a chapter-select run the list starts with
// 1 (save) and 1000 (the storage device selector) instead. 7 (with profile flag 0x40) is not known yet.
//
// Developer builds: KK_DEV_GAME_PAUSE=1 keeps the game's own pause menu; KK_DEV_PAUSE_STANDIN=1 draws a
// plain ImGui list for the port's menu until the launcher-style one (pause_menu.cpp) is in;
// KK_DEV_PAUSE_ACTIONS="<s>:<action>,..." runs pause actions at those seconds of the script clock
// (resume, again, score, quit, open = press Start), for tests.

#include "ui_paint.h"

#include <algorithm>
#include "debug_overlay.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <functional>
#include <memory>
#include <mutex>

#include <imgui.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>

#include <rex/input/mnk/mnk_input_driver.h>

#include "pause_menu.h"
#include "photo_mode.h"
#include "play_internal.h"
#include "backend/picture_adjust.h"  // brightness and gamma on the kept frame

#if defined(KK_DEV_TOOLS)
namespace kk {
void DevPageRuns(const char* name, uint8_t* base);  // dev_pause.cpp (KK_DEV_PAGE_TRACE)
void DevPressPauseStart();
}
#define KK_PAGE_TRACE(name) kk::DevPageRuns(name, base)
#else
#define KK_PAGE_TRACE(name) ((void)0)
#endif

namespace kk::play {
namespace {

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  uint8_t* p = base + addr;
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr uint32_t kAiPtr = 0x82CCBF64, kGamePtr = 0x82CC97C0;
constexpr uint32_t kStartMenuLevel = 3;
constexpr int64_t kPageGoneMs = 300;  // a page script not run for this long is gone

bool TakeoverOn() {
#if defined(KK_DEV_TOOLS)
  static const bool off = std::getenv("KK_DEV_GAME_PAUSE") != nullptr;
  return !off;
#else
  return true;
#endif
}

// The page scripts' variables (the running menu instance's) and the universe variables.
uint32_t PageVars(const uint8_t* base) {
  const uint32_t ai = Load32(base, kAiPtr);
  return ai ? Load32(base, ai + 0x40) : 0;
}
bool InPlay(const uint8_t* base) {
  const uint32_t g = Load32(base, kGamePtr);
  return g && Load32(base, g + 8) != kStartMenuLevel;
}

enum Phase { kIdle, kFindRow, kPress, kConfirm };

std::atomic<int64_t> g_pause_started_ms{-100000};  // IntMIG_wait opened the pause page
std::atomic<int64_t> g_main_seen_ms{-100000};      // the pause page last ran (we did not run its script)
std::atomic<int64_t> g_game_page_seen_ms{-100000};     // a game page (Score, Save) last ran
std::atomic<bool> g_game_page{false};               // the Score or Save item's pages are showing (the game's)
std::atomic<bool> g_has_slot{false};               // a chapter-select run: Start again and Score exist
std::atomic<bool> g_can_save{false};               // a story run with a save slot: Save exists
std::atomic<int64_t> g_game_page_since{0};         // when Score or Save opened the game's own pages
std::atomic<int64_t> g_closing_until{0};
std::atomic<bool> g_left_by_quit{false};
std::atomic<bool> g_quit_to_desktop{false};           // the page's last frames after Resume: not the menu
std::atomic<int> g_pending{-1};                    // PauseAction being carried out, -1 none
std::atomic<int> g_phase{kIdle};
std::atomic<int64_t> g_confirm_seen_ms{0};
int g_confirm_frames = 0;
int g_try_row = 0;
int g_find_rounds = 0;  // rounds of the row search that found no list yet
int g_force_button = -1;  // the game's button query answers "pressed" for this one only (99: none)
std::atomic<int> g_dev_opt_row{-1}, g_dev_opt_back{0};  // developer Options page presses

std::atomic<uint16_t> g_raw_buttons{0};
uint16_t g_hidden_mask = 0;  // buttons held as the menu closed: hidden from the game until let go
bool g_was_open = false;


bool MenuUp() { return NowMs() - g_main_seen_ms.load() < kPageGoneMs; }

int ItemId(int action) {
  switch (PauseAction(action)) {
    case PauseAction::kResume: return 4;
    case PauseAction::kStartAgain: return 8;
    case PauseAction::kScore: return 0;
    case PauseAction::kQuitToLauncher: return 3;
    case PauseAction::kSave: return 1;
    case PauseAction::kQuitToDesktop: return 3;
  }
  return action == int(PauseAction::kGameOptions) ? 2 : 4;
}
const char* ActionName(int action) {
  static const char* const kNames[] = {"Resume", "Start again", "Score", "Return to main menu", "Save",
                                       "Quit game", "Options"};
  return action >= 0 && action < 7 ? kNames[action] : "none";
}

}  // namespace

bool GamePausePageUp() { return TakeoverOn() && MenuUp() && !g_game_page && !CoverScreen(); }

// Photo mode (photo_mode.cpp) keeps the game's pause and steps the port's menu aside while it is open.
bool PauseMenuOpen() { return GamePausePageUp() && !photo::Active(); }

bool PauseActionAvailable(PauseAction action) {
  if (!PauseMenuOpen() || g_pending >= 0) return false;
  if (action == PauseAction::kStartAgain || action == PauseAction::kScore) return g_has_slot;
  if (action == PauseAction::kSave) return g_can_save;
  return true;
}

void RunPauseAction(PauseAction action) {
  if (action == PauseAction::kQuitToLauncher || action == PauseAction::kQuitToDesktop) {
    g_left_by_quit = true;
    g_quit_to_desktop = action == PauseAction::kQuitToDesktop;
  }
  if (!PauseActionAvailable(action)) {
    REXLOG_WARN("KK pause: {} is not available now", ActionName(int(action)));
    return;
  }
  int none = -1;
  if (g_pending.compare_exchange_strong(none, int(action))) {
    g_phase = kFindRow;
    REXLOG_INFO("KK pause: {} chosen", ActionName(int(action)));
  }
}

// The last frame of play (RGBA), captured as the pause starts.
std::mutex g_backdrop_mutex;
std::vector<uint8_t> g_backdrop;
int g_backdrop_w = 0, g_backdrop_h = 0, g_backdrop_version = 0;
// Brightness and gamma (Display > Picture) as they were when the frame was kept: the pause menu's sliders
// change the kept frame from these, since the game draws nothing while paused.
nr::PictureAdjust PictureNow() {
  return nr::PictureAdjust::FromSettings(std::atof(rex::cvar::GetFlagByName("kk_brightness").c_str()),
                                         std::atof(rex::cvar::GetFlagByName("kk_gamma").c_str()));
}
nr::PictureAdjust g_backdrop_picture;
std::function<bool(int&, int&, std::vector<uint8_t>&)> g_grab;

void SetFrameGrabber(std::function<bool(int&, int&, std::vector<uint8_t>&)> grab) { g_grab = std::move(grab); }

namespace {
void GrabBackdrop() {
  if (!g_grab) return;
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;
  const auto t0 = std::chrono::steady_clock::now();
  if (!g_grab(w, h, rgba)) {
    REXLOG_WARN("KK pause: the last frame could not be captured");
    return;
  }
  std::lock_guard lock(g_backdrop_mutex);
  g_backdrop = std::move(rgba);
  g_backdrop_w = w, g_backdrop_h = h;
  g_backdrop_picture = PictureNow();
  ++g_backdrop_version;
  REXLOG_INFO("KK pause: kept the last frame of play ({}x{}, {:.0f} ms)", w, h,
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

// Under every dialog (the background draw list): the kept frame, while the port's menu is open (or the
// game's confirmation page, which the port answers, is up).
class PauseBackdrop final : public rex::ui::ImGuiDialog {
 public:
  PauseBackdrop(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate)
      : ImGuiDialog(drawer), immediate_(immediate) {}

  bool NeedsContinuousPaint() override { return TakeoverOn() && !g_game_page && (MenuUp() || g_pending >= 0) && !CoverScreen(); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    // Photo mode shows the live scene instead (the game's black cover is left out by the renderer).
    const bool show = TakeoverOn() && !g_game_page && (MenuUp() || g_pending >= 0) && !CoverScreen() && !photo::Active();
    if (!show) {
      if (texture_ && !MenuUp()) texture_.reset();  // the pause is over: let the memory go
      return;
    }
    {
      std::lock_guard lock(g_backdrop_mutex);
      const nr::PictureAdjust picture = PictureNow();
      if ((g_backdrop_version != version_ || picture != picture_) && !g_backdrop.empty() && immediate_) {
        version_ = g_backdrop_version;
        picture_ = picture;
        const uint8_t* pixels = g_backdrop.data();
        std::vector<uint8_t> retouched;
        if (picture != g_backdrop_picture) {  // the sliders moved while paused: show their picture
          const auto lut = nr::PictureAdjust::RetouchTable(g_backdrop_picture, picture);
          retouched = g_backdrop;
          for (size_t i = 0; i + 3 < retouched.size(); i += 4)
            retouched[i] = lut[retouched[i]], retouched[i + 1] = lut[retouched[i + 1]],
            retouched[i + 2] = lut[retouched[i + 2]];
          pixels = retouched.data();
        }
        texture_ = immediate_->CreateTexture(uint32_t(g_backdrop_w), uint32_t(g_backdrop_h),
                                             rex::ui::ImmediateTextureFilter::kLinear, false, pixels);
        aspect_ = float(g_backdrop_w) / float(g_backdrop_h);
      }
    }
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 255));
    if (!texture_) return;
    // Fitted like the game's output (letterboxed to its aspect ratio).
    float w = io.DisplaySize.x, h = w / aspect_;
    if (h > io.DisplaySize.y) h = io.DisplaySize.y, w = h * aspect_;
    const ImVec2 p0((io.DisplaySize.x - w) * 0.5f, (io.DisplaySize.y - h) * 0.5f);
    dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(texture_.get())), p0, ImVec2(p0.x + w, p0.y + h));
  }

 private:
  rex::ui::ImmediateDrawer* immediate_;
  std::unique_ptr<rex::ui::ImmediateTexture> texture_;
  int version_ = 0;
  nr::PictureAdjust picture_;
  float aspect_ = 16.0f / 9.0f;
};
}  // namespace

void CreatePauseBackdrop(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate) {
  new PauseBackdrop(drawer, immediate);  // lives for the rest of the session
}

void FilterPad(uint32_t user, uint8_t* state) {
  const uint16_t buttons = uint16_t(state[4] << 8 | state[5]);
  if (user == 0) g_raw_buttons = buttons;
  const bool menu = PauseMenuOpen();
  // The port's menu reads the pad the game read (pause_menu.cpp); keyboard and mouse reach it through
  // ImGui, so the mouse is let go (no mouse look) while it is open.
  // Buttons already held as the menu opens (the Start that paused) reach it only once let go.
  static bool was_menu = false;
  static uint16_t held_at_open = 0;
  if (user == 0) {
    if (menu && !was_menu) held_at_open = buttons;
    held_at_open &= buttons;
    if (menu != was_menu) kk::RequestUiPaint();
    was_menu = menu;
  }
  if (user == 0 && menu) {
    auto s16 = [&](int at) { return int16_t(uint16_t(state[at] << 8 | state[at + 1])); };
    kk::PauseMenuPad(uint16_t(buttons & ~held_at_open), s16(8), s16(10), state[6], state[7]);
  }
  static bool look_off = false;
  const bool mouse_ui = menu || kk::DebugOverlayOpen();
  if (mouse_ui != look_off) {
    look_off = mouse_ui;
    rex::input::mnk::SetMouseLookActive(!mouse_ui);
  }
  const bool open = menu || (CoverScreen() && !BonusMovieActive()) || (g_pending >= 0 && MenuUp());
  if (open) {
    g_was_open = true;
    g_hidden_mask = 0xFFFF;
    std::memset(state + 4, 0, 12);  // no buttons, triggers or sticks
    return;
  }
  if (g_was_open) {  // just closed: whatever is still held stays hidden until let go
    g_was_open = false;
    g_hidden_mask = buttons;
  }
  g_hidden_mask &= buttons;
  if (g_hidden_mask) {
    const uint16_t shown = buttons & ~g_hidden_mask;
    state[4] = uint8_t(shown >> 8), state[5] = uint8_t(shown);
  }
}

bool LeftByQuit() { return g_left_by_quit; }
bool QuitToDesktop() { return g_quit_to_desktop; }

void ResetPause() {
  g_pending = -1;
  g_phase = kIdle;
  g_game_page = false;
  g_main_seen_ms = -100000;
  g_pause_started_ms = -100000;
}

#if defined(KK_DEV_TOOLS)
uint16_t DevRawButtons() { return g_raw_buttons; }
#endif

}  // namespace kk::play

// IntMIG_wait: runs every frame in play; Start opens the pause page (its variable +0x1C becomes 1).
REX_EXTERN(__imp__sub_824B44E8);
REX_EXTERN(__imp__sub_8272B7E8);  // the game's button query (hooked below)
REX_HOOK_RAW(sub_824B44E8) {
  using namespace kk::play;
  KK_PAGE_TRACE("IntMIG_wait");
  const uint32_t v = PageVars(base);
  const uint32_t before = v ? Load32(base, v + 0x1C) : 0;
  // A gallery from Extras: on Start, IntMIG_wait leaves the bonus when G+0x5680 (the Extras row the
  // original records to come back to its menu) is set and G+0x5674 (the alternate ending) is not, instead of
  // opening its pause page (sub_824B44E8 at 0x824B4D04). That threw the player out to Extras with no pause
  // menu (task 204). The row is hidden from that one check, so Start opens the game's pause page as in any
  // level and the port's menu takes over; the row stays for the bonus's own end (its restore path).
  const uint32_t g = Load32(base, kGamePtr);
  uint32_t row = 0;
  if (g && v && BonusLevelSession() && TakeoverOn() && InPlay(base) && !before && Load32(base, g + 0x5674) == 0 &&
      (row = Load32(base, g + 0x5680)) != 0) {
    PPCContext query = ctx;
    query.r3.u64 = 9;  // Start, pressed this frame (the same query IntMIG_wait makes)
    __imp__sub_8272B7E8(query, base);
    if (query.r3.u32 && g_force_button < 0) {
      Store32(base, g + 0x5680, 0);
      REXLOG_INFO("KK pause: Start in a bonus gallery opens the pause menu (Extras row {} kept aside)", row);
    } else {
      row = 0;
    }
  }
  __imp__sub_824B44E8(ctx, base);
  if (row) Store32(base, g + 0x5680, row);
  // Play goes on: whatever the port's menu was doing is over (Start again restarts the chapter without
  // a level load; its confirmation page does not come back to the pause page).
  if (v && !Load32(base, v + 0x1C) && g_phase == kConfirm && g_confirm_frames > 0 &&
      NowMs() - g_confirm_seen_ms.load() > kPageGoneMs) {
    REXLOG_INFO("KK pause: play goes on after the confirmation page ({})", ActionName(g_pending));
    g_confirm_frames = 0;
    g_pending = -1;
    g_phase = kIdle;
  }
  // The game's own pages (Score, Save, Options) can end in play rather than back on the pause page
  // (Save resumes the game once it has saved).
  if (v && !Load32(base, v + 0x1C) && g_game_page && NowMs() - g_game_page_since.load() > 500) {
    REXLOG_INFO("KK pause: play goes on after the game's own page");
    g_game_page = false;
  }
  if (v && !before && Load32(base, v + 0x1C) && InPlay(base) && TakeoverOn()) {
    g_pause_started_ms = NowMs();
    REXLOG_INFO("KK pause: the game paused; the port's pause menu takes over");
    GrabBackdrop();  // the frame on screen is still the last one of play
  }
}

// The game's button query (pressed this frame): button r3 of the active pad.
REX_HOOK_RAW(sub_8272B7E8) {
  if (kk::play::g_force_button >= 0) {
    ctx.r3.u64 = int(ctx.r3.u32) == kk::play::g_force_button ? 1 : 0;
    return;
  }
  __imp__sub_8272B7E8(ctx, base);
}

// The pause page (IntMIG_Page_Main; also the start menu's main page, left alone there).
REX_EXTERN(__imp__sub_824BA560);
REX_HOOK_RAW(sub_824BA560) {
  using namespace kk::play;
  KK_PAGE_TRACE("IntMIG_Page_Main");
  ServiceProfileOptions(ctx, base);
  const uint32_t v = PageVars(base);
  if (!TakeoverOn() || !InPlay(base) || !v) {
    __imp__sub_824BA560(ctx, base);
    return;
  }
  const int64_t now = NowMs();
  if (now < g_closing_until.load()) return;  // the page is closing after Resume (its script is not run)
  if (now - g_main_seen_ms.load() > 1000 && !g_game_page) {
    g_pending = -1;
    g_phase = kIdle;
    REXLOG_INFO("KK pause: the port's pause menu is open (the game's page is not run)");
  }
  const bool was_visible = PauseMenuOpen();
  g_main_seen_ms = now;
  if (PauseMenuOpen() != was_visible) kk::RequestUiPaint();
  const uint32_t g = Load32(base, kGamePtr);
  const bool slot = int32_t(Load32(base, g + 0x4E10)) != -1, chapter_run = Load32(base, g + 0x5668) != 0;
  g_has_slot = slot && chapter_run;
  g_can_save = slot && !chapter_run && Load32(base, g + 0x5674) == 0;
  // Back from the game's own pages (Score, Save, Options): while they show, this page is not run; it runs
  // again when they close (after its own last frames right after the press).
  if (g_game_page && now - g_game_page_seen_ms.load() > kPageGoneMs && now - g_game_page_since.load() > 500) {
    g_game_page = false;
    REXLOG_INFO("KK pause: back from the game's own page");
  }
  if (g_game_page) return;
  const int pending = g_pending;
  if (g_phase == kConfirm) {
    // The game's confirmation page runs on top (and this page under it) until it acts.
    if (g_confirm_frames > 0 && now - g_confirm_seen_ms.load() > kPageGoneMs) {
      REXLOG_INFO("KK pause: the confirmation page is gone after {} frames ({})", g_confirm_frames,
                  ActionName(pending));
      g_confirm_frames = 0;
      g_pending = -1;
      g_phase = kIdle;
    }
    return;
  }
  if (pending < 0) return;  // the menu is up: the game's page does not run
  const int want = ItemId(pending);
  if (g_phase == kFindRow) {
    // Find the wanted item's row: the page keeps the selected item's id at +0x44.
    if (g_try_row > 8) g_try_row = 0;
    Store32(base, v + 0x3C, uint32_t(g_try_row));
    g_force_button = 99;
    __imp__sub_824BA560(ctx, base);
    g_force_button = -1;
    const int32_t id = int32_t(Load32(base, v + 0x44));
    REXLOG_INFO("KK pause: row {} of the game's pause page is item {}", g_try_row, id);
    if (id == want) {
      g_phase = kPress;
      g_find_rounds = 0;
    } else if (++g_try_row > 8) {
      // Right after it opens the page has no list yet (every row -1): try again for up to 2 s.
      g_try_row = 0;
      if (id != -1 || ++g_find_rounds > 7) {
        REXLOG_WARN("KK pause: the game's pause page has no item {}", want);
        g_pending = -1;
        g_phase = kIdle;
        g_find_rounds = 0;
      }
    }
    return;
  }
  // kPress: run the page with that row selected and A pressed.
  Store32(base, v + 0x3C, uint32_t(g_try_row));
  g_force_button = 0;
  __imp__sub_824BA560(ctx, base);
  g_force_button = -1;
  REXLOG_INFO("KK pause: ran the game's pause page with item {} (row {}) and A", want, g_try_row);
  g_try_row = 0;
  switch (PauseAction(pending)) {
    case PauseAction::kResume:
      g_pending = -1;
      g_phase = kIdle;
      g_main_seen_ms = -100000;
      g_pause_started_ms = -100000;
      g_closing_until = now + 1000;
      break;
    case PauseAction::kScore:
    case PauseAction::kGameOptions:
      g_game_page = true;
      g_game_page_seen_ms = now;
      g_game_page_since = now;
      g_pending = -1;
      g_phase = kIdle;
      break;
    default:
      g_phase = kConfirm;  // the game's confirmation page comes next
      g_confirm_frames = 0;
      break;
  }
}

// The confirmation page (IntMIG_Page_MessageYesNo): Yes for the port's choice.
REX_EXTERN(__imp__sub_824BC1B0);
REX_HOOK_RAW(sub_824BC1B0) {
  using namespace kk::play;
  KK_PAGE_TRACE("IntMIG_Page_MessageYesNo");
  const int pending = g_pending;
  if (!TakeoverOn() || !InPlay(base) || g_phase != kConfirm ||
      (pending != int(PauseAction::kStartAgain) && pending != int(PauseAction::kQuitToLauncher) &&
       pending != int(PauseAction::kQuitToDesktop) && pending != int(PauseAction::kSave))) {
    __imp__sub_824BC1B0(ctx, base);
    return;
  }
  g_main_seen_ms = NowMs();
  g_confirm_seen_ms = NowMs();
  // The page takes input only after its opening delay: answer A (its default, Yes) every frame it runs
  // from the third on. The pause page's hook ends this once the confirmation page has stopped running.
  const int frames = ++g_confirm_frames;
  g_force_button = frames < 3 ? 99 : 0;
  __imp__sub_824BC1B0(ctx, base);
  g_force_button = -1;
  if (frames == 3) REXLOG_INFO("KK pause: answering Yes on the game's confirmation page ({})", ActionName(pending));
}

// The game's score pages (IntMIG_Page_ScoreNew): while the Score item's page shows, the game draws and
// reads the pad as usual.
REX_EXTERN(__imp__sub_824D1958);
REX_HOOK_RAW(sub_824D1958) {
  using namespace kk::play;
  KK_PAGE_TRACE("IntMIG_Page_ScoreNew");
  if (g_game_page) g_game_page_seen_ms = NowMs();
  __imp__sub_824D1958(ctx, base);
}

// ------------------------------------------------------------------- developer stand-in and test aid ---
#if defined(KK_DEV_TOOLS)

#include <sstream>

#include <rex/cvar.h>

namespace kk {
double DevSecondsSinceMenu();  // dev_tools.cpp: the script clock, -1 before it starts
}

namespace kk::play {
namespace {

bool StandInOn() {
  static const bool on = std::getenv("KK_DEV_PAUSE_STANDIN") != nullptr;
  return on;
}

struct Item {
  const char* label;
  PauseAction action;
};
std::vector<Item> Items() {
  std::vector<Item> items;
  for (const Item& i : {Item{"Resume", PauseAction::kResume}, Item{"Start again", PauseAction::kStartAgain},
                        Item{"Score", PauseAction::kScore}, Item{"Quit game", PauseAction::kQuitToLauncher}})
    if (i.action == PauseAction::kResume || i.action == PauseAction::kQuitToLauncher || g_has_slot) items.push_back(i);
  return items;
}

class PauseStandIn final : public rex::ui::ImGuiDialog {
 public:
  explicit PauseStandIn(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const bool open = PauseMenuOpen();
    if (open != was_open_) {
      REXLOG_INFO("KK pause stand-in: {}", open ? "drawn" : "gone");
      // The press that opened the menu (Start) does not count.
      last_ = DevRawButtons();
      sel_ = 0;
    }
    was_open_ = open;
    if (!open) return;
    const auto items = Items();
    // The controller: up / down move, A chooses, B or Start resume.
    const uint16_t buttons = DevRawButtons(), pressed = buttons & ~last_;
    last_ = buttons;
    if (pressed & 0x0001) sel_ = (sel_ + int(items.size()) - 1) % int(items.size());
    if (pressed & 0x0002) sel_ = (sel_ + 1) % int(items.size());
    sel_ = std::min(sel_, int(items.size()) - 1);
    if (pressed & 0x1000) RunPauseAction(items[size_t(sel_)].action);
    if (pressed & (0x2000 | 0x0010)) RunPauseAction(PauseAction::kResume);
    const float s = ImGui::GetFontSize() / 18.0f;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));  // dim the frozen frame
    const ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    const float row = 44 * s, w = 360 * s, h = row * float(items.size()) + 90 * s;
    const ImVec2 p0(c.x - w / 2, c.y - h / 2), p1(c.x + w / 2, c.y + h / 2);
    dl->AddRectFilled(p0, p1, IM_COL32(16, 22, 40, 225), 14 * s);
    dl->AddText(nullptr, 26 * s, ImVec2(p0.x + 28 * s, p0.y + 20 * s), IM_COL32(235, 235, 245, 255),
                "Paused (stand-in)");
    for (size_t i = 0; i < items.size(); ++i) {
      const ImVec2 r0(p0.x + 20 * s, p0.y + 70 * s + row * float(i)), r1(p1.x - 20 * s, r0.y + row - 6 * s);
      const bool sel = int(i) == sel_;
      if (sel) dl->AddRectFilled(r0, r1, IM_COL32(70, 110, 200, 230), 8 * s);
      dl->AddText(nullptr, 22 * s, ImVec2(r0.x + 16 * s, r0.y + 9 * s),
                  sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(190, 195, 210, 255), items[i].label);
    }
  }

 private:
  bool was_open_ = false;
  uint16_t last_ = 0;
  int sel_ = 0;
};

// KK_DEV_PAUSE_ACTIONS="40:open,45:score,52:resume,...": test steps on the script clock (seconds since the
// start request's level loaded). "open" asks the pad for Start (through the developer pad, see below).
struct Step {
  double at;
  std::string what;
};
std::vector<Step> ParseSteps() {
  std::vector<Step> steps;
  const char* v = std::getenv("KK_DEV_PAUSE_ACTIONS");
  if (!v || !*v) return steps;
  std::stringstream all(v);
  for (std::string t; std::getline(all, t, ',');) {
    const size_t colon = t.find(':');
    if (colon != std::string::npos) steps.push_back({std::atof(t.substr(0, colon).c_str()), t.substr(colon + 1)});
  }
  return steps;
}

const bool g_steps_thread = [] {
  auto steps = ParseSteps();
  if (steps.empty()) return false;
  std::thread([steps] {
    size_t next = 0;
    while (next < steps.size()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      const double t = kk::DevSecondsSinceMenu();
      if (t < 0 || t < steps[next].at) continue;
      const std::string& w = steps[next].what;
      if (w == "open" || w == "close") {
        const bool open = PauseMenuOpen();
        REXLOG_INFO("KK dev pause step {:.1f} s: {} (menu open {})", t, w, open);
        if (w == "open" && !open) kk::DevPressPauseStart();
        if (w == "close" && open) RunPauseAction(PauseAction::kResume);
        ++next;
        continue;
      }
      if (w.rfind("optpick=", 0) == 0) {
        g_dev_opt_row = std::atoi(w.c_str() + 8);
        ++next;
        continue;
      }
      if (w == "optback") {
        g_dev_opt_back = 1;
        ++next;
        continue;
      }
      // "set=<cvar>=<value>": change a setting as the pause menu's Settings would (live settings tests).
      if (w.rfind("set=", 0) == 0) {
        const size_t eq = w.find('=', 4);
        if (eq != std::string::npos) {
          rex::cvar::SetFlagByName(w.substr(4, eq - 4), w.substr(eq + 1));
          REXLOG_INFO("KK dev pause step {:.1f} s: {} = {}", t, w.substr(4, eq - 4), w.substr(eq + 1));
        }
        ++next;
        continue;
      }
      PauseAction a = PauseAction::kResume;
      bool known = true;
      if (w == "resume") a = PauseAction::kResume;
      else if (w == "again") a = PauseAction::kStartAgain;
      else if (w == "score") a = PauseAction::kScore;
      else if (w == "quit") a = PauseAction::kQuitToLauncher;
      else if (w == "desktop") a = PauseAction::kQuitToDesktop;
      else if (w == "save") a = PauseAction::kSave;
      else if (w == "options") a = PauseAction::kGameOptions;
      else known = false;
      REXLOG_INFO("KK dev pause step {:.1f} s: {} (menu open {}, available {})", t, w, PauseMenuOpen(),
                  known && PauseActionAvailable(a));
      if (known) RunPauseAction(a);
      ++next;
    }
  }).detach();
  return true;
}();

}  // namespace
}  // namespace kk::play

namespace kk {
void CreatePauseStandIn(rex::ui::ImGuiDrawer* drawer) {
  if (play::StandInOn()) new play::PauseStandIn(drawer);
}
}  // namespace kk

#endif

// The save menu page (IntMIG_Page_Memcard): in play it shows the game's save pages (Save, and the save that
// Quit game makes in a story run). They are the game's: shown and answered by the player, the port's menu
// steps aside (and comes back if the pause page does).
REX_EXTERN(__imp__sub_824CB738);
REX_HOOK_RAW(sub_824CB738) {
  using namespace kk::play;
  KK_PAGE_TRACE("IntMIG_Page_Memcard");
  if (TakeoverOn() && InPlay(base) && (g_pending >= 0 || g_game_page)) {
    const int64_t now = NowMs();
    if (!g_game_page) {
      REXLOG_INFO("KK pause: the game's save page is up ({}); the port's menu steps aside", ActionName(g_pending));
      g_game_page = true;
      g_game_page_since = now;
      g_pending = -1;
      g_phase = kIdle;
      g_confirm_frames = 0;
    }
    g_game_page_seen_ms = now;
  }
  __imp__sub_824CB738(ctx, base);
}

// The game's Options pages (IntMIG_Page_OptStart and its Audio, Video, Controls and Language pages): the
// game's own pages, shown and read by the game while they are up.
#if defined(KK_DEV_TOOLS)
namespace kk::play {
// KK_DEV_PAUSE_ACTIONS "optpick=<row>" / "optback": on the next run of an Options page, select that row and
// press A, or press B (the developer pad cannot move through these pages).
void DevOptionsPage(uint8_t* base) {
  const int row = g_dev_opt_row.exchange(-1);
  if (row >= 0) {
    if (const uint32_t v = PageVars(base)) Store32(base, v + 0x3C, uint32_t(row));
    g_force_button = 0;
    REXLOG_INFO("KK dev pause: Options page row {} and A", row);
  } else if (g_dev_opt_back.exchange(0)) {
    g_force_button = 3;
    REXLOG_INFO("KK dev pause: Options page B");
  }
}
}  // namespace kk::play
#define KK_DEV_OPTIONS_PAGE() kk::play::DevOptionsPage(base)
#else
#define KK_DEV_OPTIONS_PAGE() ((void)0)
#endif
#define KK_GAME_PAGE_HOOK(addr, name)                                   \
  REX_EXTERN(__imp__sub_##addr);                                        \
  REX_HOOK_RAW(sub_##addr) {                                            \
    using namespace kk::play;                                           \
    KK_PAGE_TRACE(name);                                                \
    if (g_game_page) g_game_page_seen_ms = NowMs();                     \
    KK_DEV_OPTIONS_PAGE();                                              \
    __imp__sub_##addr(ctx, base);                                       \
    g_force_button = -1;                                                \
  }
KK_GAME_PAGE_HOOK(824C0F60, "IntMIG_Page_OptStart")
KK_GAME_PAGE_HOOK(824CD320, "IntMIG_Page_OptStart_Audio")
KK_GAME_PAGE_HOOK(824CE4A0, "IntMIG_Page_OptStart_Video")
KK_GAME_PAGE_HOOK(824CF400, "IntMIG_Page_OptStart_Control")
KK_GAME_PAGE_HOOK(824D2F20, "IntMIG_Page_OptStart_Langue")
