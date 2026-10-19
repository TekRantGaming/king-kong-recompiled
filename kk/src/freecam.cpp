#include "freecam.h"
#include "debug_overlay.h"
#include "freecam_math.h"
#include "photo_mode.h"
#include "play_internal.h"
#include "ps2_movies.h"
#include "settings.h"
#include "ui_paint.h"
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

REXCVAR_DEFINE_BOOL(kk_freecam, true, "KK/Debug",
                    "F9 free camera and F10 HUD toggle (on by default since 17 Oct, owner decision)")
    .debug_only();
REXCVAR_DEFINE_BOOL(kk_hide_hud, false, "KK",
                    "Hide the game's HUD all the time (saved; the Hardcore mod). F10 still hides the rest for "
                    "screenshots. The weapon in Jack's hands stays");

namespace kk::freecam {
namespace {
using Clock = std::chrono::steady_clock;
std::atomic<bool> active{false}, hidden{false};
std::atomic<uint32_t> generation{0};
// Photo mode (photo_mode.cpp): the camera inside it, its limits and what the mouse does.
std::atomic<bool> photo{false}, photo_mouse{false};
std::atomic<float> photo_roll{0}, photo_speed{3};
float photo_radius = 15;
Vec photo_center;
std::mutex input_mutex;
bool keys[256]{};
double mouse_x = 0, mouse_y = 0, wheel = 0;
std::array<PadInput, 4> pads{};
#if defined(KK_DEV_TOOLS)
std::array<uint8_t, 16> test_pad{};
std::atomic<unsigned> trace_stage{0};
std::atomic<uint64_t> pad_polls{0}, pad_leaks{0};
#endif
Camera camera;
uint32_t captured_generation = 0, level = 0;
bool initialized = false;
Clock::time_point last_frame;
uint64_t frame = 0, moved_frame = ~uint64_t(0);
float real_dt = 0;
bool InPlay() {
  return play::SessionActive() && !play::CoverScreen() &&
         !play::PauseMenuOpen();
}
uint32_t U32(const uint8_t *b, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, b + a, 4);
  return std::byteswap(v);
}
float F32(const uint8_t *b, uint32_t a) {
  return std::bit_cast<float>(U32(b, a));
}
void Put(uint8_t *b, uint32_t a, float f) {
  auto v = std::byteswap(std::bit_cast<uint32_t>(f));
  std::memcpy(b + a, &v, 4);
}
Vec ReadVec(const uint8_t *b, uint32_t a) {
  return {F32(b, a), F32(b, a + 4), F32(b, a + 8)};
}
void WriteVec(uint8_t *b, uint32_t a, Vec v) {
  Put(b, a, v.x);
  Put(b, a + 4, v.y);
  Put(b, a + 8, v.z);
}
// Photo mode's pad: the buttons to its panel (the first player's), the rest for the camera.
PhotoPadExtra ReadPhotoPad(const uint8_t *state, uint32_t user, bool live) {
  if (user == 0 && live) {
    auto s16 = [&](int at) { return int16_t(uint16_t(state[at] << 8 | state[at + 1])); };
    kk::photo::OnPad(uint16_t(state[4] << 8 | state[5]), s16(8), s16(10), s16(12), s16(14), state[6], state[7]);
  }
  return PhotoPadFromState(state);
}
void ClearInput() {
  std::lock_guard lock(input_mutex);
  std::memset(keys, 0, sizeof(keys));
  mouse_x = mouse_y = wheel = 0;
  pads = {};
}
void Toggle() {
  active = !active.load();
  ++generation;
  ClearInput();
  RequestUiPaint();
  REXLOG_INFO("KK freecam: {}", active.load()
                                    ? "enabled"
                                    : "disabled; normal game camera restored");
}
bool MotionKey(unsigned k) {
  // Photo mode: Q and E switch the panel's tabs (Space and Ctrl still go up and down).
  if (photo && (k == 'Q' || k == 'E')) return false;
  return k == 'W' || k == 'A' || k == 'S' || k == 'D' || k == 'E' || k == 'Q' ||
         k == 0x20 || k == 0x10 || k == 0x11 || k == 0x12 ||
         (k >= 0xA0 && k <= 0xA5);
}
class Controls final : public rex::ui::ImGuiDialog,
                       public rex::ui::WindowInputListener,
                       public rex::ui::WindowListener {
public:
  Controls(rex::ui::ImGuiDrawer *d, rex::ui::Window *w)
      : ImGuiDialog(d), window(w) {
    window->AddInputListener(this, 1250);
    window->AddListener(this);
  }
  ~Controls() override {
    window->RemoveInputListener(this);
    window->RemoveListener(this);
  }
  bool NeedsContinuousPaint() override {
#if defined(KK_DEV_TOOLS)
    if (std::getenv("KK_DEV_PUBLIC_DEFAULT_TEST") && !defaults_tested)
      return true;
    if (std::getenv("KK_DEV_FREECAM_TEST") && test_stage < 36)
      return true;
#endif
    return active.load();
  }
  void OnLostFocus(rex::ui::UISetupEvent &) override { ClearInput(); }
  void OnKeyDown(rex::ui::KeyEvent &e) override { Key(e, true); }
  void OnKeyUp(rex::ui::KeyEvent &e) override { Key(e, false); }
  void OnMouseMove(rex::ui::MouseEvent &e) override {
    if (!active || DebugOverlayOpen() || !InPlay() || (photo && !photo_mouse))
      return;
    const double density = window->GetMediumDpi() ? double(window->GetDpi()) /
                                                        window->GetMediumDpi()
                                                  : 1;
    std::lock_guard lock(input_mutex);
    mouse_x += e.dx() / std::max(.25, density);
    mouse_y += e.dy() / std::max(.25, density);
    e.set_handled(true);
  }
  void OnMouseWheel(rex::ui::MouseEvent &e) override {
    if (!active || DebugOverlayOpen() || !InPlay() || (photo && !photo_mouse))
      return;
    std::lock_guard lock(input_mutex);
    wheel += double(e.scroll_y()) / rex::ui::MouseEvent::kScrollPerDetent;
    e.set_handled(true);
  }
  // Photo mode leaves the buttons to its panel (the right button held turns the camera there).
  void OnMouseDown(rex::ui::MouseEvent &e) override {
    if (active && !DebugOverlayOpen() && !photo)
      e.set_handled(true);
  }
  void OnMouseUp(rex::ui::MouseEvent &e) override {
    if (active && !DebugOverlayOpen() && !photo)
      e.set_handled(true);
  }

protected:
  void OnDraw(ImGuiIO &) override {
#if defined(KK_DEV_TOOLS)
    if (std::getenv("KK_DEV_PUBLIC_DEFAULT_TEST") && !defaults_tested && InPlay()) {
      defaults_tested = true;
      // F9/F10 are public since 17 Oct (owner): on by default, idle until pressed, and each key
      // toggles its feature on and back off.
      const bool idle = REXCVAR_GET(kk_freecam) && !active && !hidden;
      auto press = [&](unsigned k) {
        rex::ui::KeyEvent e(window, rex::ui::VirtualKey(k), 1, false, false,
                            false, false, false);
        Key(e, true);
      };
      press(0x78u);
      press(0x79u);
      const bool on = active && hidden;
      press(0x78u);
      press(0x79u);
      const bool off = !active && !hidden;
      const bool pass = idle && on && off &&
                        !REXCVAR_GET(kk_ps2_cutscenes) && movies::Folder().empty();
      REXLOG_INFO("KK public defaults test: {} F9/F10 work (idle {}, on {}, off {}), movies off, folder empty",
                  pass ? "PASS" : "FAIL", idle, on, off);
    }
    TestTick();
#endif
    const bool want =
        active && InPlay() && !DebugOverlayOpen() && window->HasFocus() && (!photo || photo_mouse);
    if (want != relative) {
      relative = want;
      const bool normal = !photo && rex::cvar::GetFlagByName("mnk_mode") == "true" &&
                          rex::cvar::GetFlagByName("mnk_mouse") == "true" &&
                          InPlay() && !DebugOverlayOpen() && window->HasFocus();
      window->SetRelativeMouseMode(want || normal);
    }
  }

private:
#if defined(KK_DEV_TOOLS)
  bool defaults_tested = false;
  void TestTick() {
    if (!std::getenv("KK_DEV_FREECAM_TEST") || !InPlay())
      return;
    if (test_start == Clock::time_point{}) test_start = Clock::now();
    const double t = std::chrono::duration<double>(Clock::now() - test_start).count();
    const double at = test_stage == 0 ? 4 : test_stage <= 32
        ? 5 + ((test_stage - 1) / 2) * 1.5 + (test_stage % 2 ? 0 : 1)
        : test_stage == 33 ? 29.5 : test_stage == 34 ? 31 : 32;
    if (test_stage >= 36 || t < at) return;
    auto key = [&](unsigned k, bool down) {
      rex::ui::KeyEvent e(window, rex::ui::VirtualKey(k), 1, false, false,
                          false, false, false);
      Key(e, down);
    };
    auto tap = [&](unsigned k) { key(k, true); key(k, false); };
    if (test_stage == 0 || test_stage == 34) {
      tap(0x78);
    } else if (test_stage <= 32) {
      const unsigned i = (test_stage - 1) / 2;
      const bool down = test_stage % 2;
      constexpr unsigned keyboard[] = {'W', 'S', 'A', 'D', 0x20, 0x11};
      if (i < 6) {
        key(keyboard[i], down);
      } else {
        std::lock_guard lock(input_mutex);
        test_pad = {};
        if (down) {
          if (i == 10 || i == 11) test_pad[4] = i == 10 ? 0x10 : 0x20;
          else {
            const int offset = i < 8 ? 10 : i < 10 ? 8 : i < 14 ? 12 : 14;
            const int16_t v = (i == 7 || i == 8 || i == 12 || i == 15)
                                  ? -32767 : 32767;
            test_pad[offset] = uint16_t(v) >> 8;
            test_pad[offset + 1] = uint8_t(v);
          }
        }
      }
    } else if (test_stage == 33) {
      rex::ui::MouseEvent e(window, rex::ui::MouseEvent::Button::kNone,
                            0, 0, 0, 0, 100, -50);
      OnMouseMove(e);
    } else {
      REXLOG_INFO("KK freecam test: DONE active={} hidden={}", active.load(), hidden.load());
    }
    REXLOG_INFO("KK freecam test: stage {} at {:.3f}", test_stage, t);
    trace_stage = ++test_stage;
  }
  unsigned test_stage = 0;
  Clock::time_point test_start;
#endif
  void Key(rex::ui::KeyEvent &e, bool down) {
    if (!REXCVAR_GET(kk_freecam))
      return;
    const unsigned k = unsigned(e.virtual_key());
    if (k == 0x78 || k == 0x79) {
      if (photo) {  // photo mode has its own camera and hides the HUD itself
        e.set_handled(true);
        return;
      }
      if (!InPlay())
        return;
      if (down && !e.prev_state()) {
        if (k == 0x78)
          Toggle();
        else {
          hidden = !hidden.load();
          RequestUiPaint();
          REXLOG_INFO("KK freecam: HUD {}",
                      hidden.load() ? "hidden" : "visible");
        }
      }
      e.set_handled(true);
      return;
    }
    if (active && MotionKey(k)) {
      std::lock_guard lock(input_mutex);
      keys[k] = down;
      e.set_handled(true);
    }
  }
  rex::ui::Window *window;
  bool relative = false;
};
} // namespace
void Install(rex::ui::ImGuiDrawer *d, rex::ui::Window *w) {
  bool test_defaults = false;
#if defined(KK_DEV_TOOLS)
  test_defaults = std::getenv("KK_DEV_PUBLIC_DEFAULT_TEST") != nullptr;
#endif
  if (!REXCVAR_GET(kk_freecam) && !test_defaults)
    return;
  new Controls(d, w);
}
bool Active() { return active.load(); }
void PhotoStart(float radius) {
  photo_radius = std::max(radius, 1.f);
  photo_roll = 0;
  photo_mouse = false;
  photo = true;
  active = true;
  ++generation;
  ClearInput();
  RequestUiPaint();
  REXLOG_INFO("KK freecam: photo mode camera on (radius {:.1f} m)", photo_radius);
}
void PhotoStop() {
  if (!photo.exchange(false)) return;
  active = false;
  photo_mouse = false;
  ++generation;
  ClearInput();
  RequestUiPaint();
  REXLOG_INFO("KK freecam: photo mode camera off; normal game camera restored");
}
void PhotoParams(float roll, float speed, bool mouse_look) {
  photo_roll = roll;
  photo_speed = std::clamp(speed, 0.1f, 100.f);
  photo_mouse = mouse_look;
}
void PhotoResetView() {
  ++generation;
}
bool PhotoActive() { return photo.load(); }
bool HideHud() { return (hidden.load() && InPlay()) || photo.load(); }
// Also under the port's pause menu (task 243): its kept frame of the paused game is taken again when the field
// of view changes there, and must look like play (the game's black cover hides the rest meanwhile).
bool HideGameHud() {
  return ((hidden.load() || REXCVAR_GET(kk_hide_hud)) && (InPlay() || play::PauseMenuOpen())) || photo.load();
}
void Pad(uint32_t user, uint8_t *state, bool connected) {
  if (user >= pads.size()) {
    std::memset(state + 4, 0, 12);
    return;
  }
  std::lock_guard lock(input_mutex);
  // Photo mode: the buttons go to its panel; the triggers go down / up, the stick clicks faster / slower.
  PhotoPadExtra extra;
  if (photo) extra = ReadPhotoPad(state, user, connected && InPlay());
  auto input = ConsumePad(
      state, std::clamp(REXCVAR_GET(kk_deadzone), 0, 50) / 100.f,
      REXCVAR_GET(kk_invert_rs_x), REXCVAR_GET(kk_invert_rs_y));
#if defined(KK_DEV_TOOLS)
  if (std::getenv("KK_DEV_FREECAM_TEST")) {
    ++pad_polls;
    for (int i = 4; i < 16; ++i) if (state[i]) ++pad_leaks;
  }
#endif
  if (photo) ApplyPhotoPad(extra, input);
  pads[user] = connected && InPlay() && !DebugOverlayOpen() ? input : PadInput{};
}
#if defined(KK_DEV_TOOLS)
void DevToggle() { Toggle(); }
bool DevPadInput(uint32_t user, uint8_t *state) {
  if (!std::getenv("KK_DEV_FREECAM_TEST") || user != 0 || !active)
    return false;
  std::lock_guard lock(input_mutex);
  std::memcpy(state, test_pad.data(), test_pad.size());
  return true;
}
#endif
void Frame(uint8_t *base) {
  const auto now = Clock::now();
  real_dt = last_frame == Clock::time_point{}
                ? 0
                : std::chrono::duration<float>(now - last_frame).count();
  last_frame = now;
  ++frame;
  const uint32_t g = U32(base, 0x82CC97C0),
                 current = g ? U32(base, g + 0x18) : 0;
  if (current != level || play::CoverScreen()) {
    if (active.exchange(false)) {
      ++generation;
      ClearInput();
      REXLOG_INFO("KK freecam: reset for level transition");
    }
    initialized = false;
    level = current;
  }
}
bool OverrideView(uint8_t *base, uint32_t view, uint32_t caller,
                  std::array<uint8_t, 68> &saved) {
  // These three calls set the same active scene view, before culling, opaque
  // geometry and effects. Other calls restore an earlier view or set up a
  // 2D/weapon view; leave those alone.
  if (!active || !InPlay() || view < 0x40000000 || view > 0xA0000000 ||
      (caller != 0x82873750 && caller != 0x8287390C && caller != 0x8287397C))
    return false;
  const uint32_t gen = generation.load();
  if (!initialized || captured_generation != gen) {
    camera.position = ReadVec(base, view + 48);
    const Vec f = ReadVec(base, view + 16);
    if (!std::isfinite(Length(camera.position)) || !std::isfinite(Length(f)) ||
        Length(f) < .5f)
      return false;
    camera.yaw = std::atan2(f.y, f.x);
    camera.pitch = std::asin(std::clamp(f.z / Length(f), -1.f, 1.f));
    if (photo) photo_center = camera.position;
    initialized = true;
    captured_generation = gen;
    moved_frame = frame;
    REXLOG_INFO("KK freecam: captured game view; position {:.3f} {:.3f} {:.3f}",
                camera.position.x, camera.position.y, camera.position.z);
  }
  if (moved_frame != frame) {
    moved_frame = frame;
    std::lock_guard lock(input_mutex);
    if (!DebugOverlayOpen()) {
      const float sensitivity =
          float(std::clamp(REXCVAR_GET(kk_mouse_sensitivity), .05, 10.0) *
                .0011519173);
      camera.Look(float(mouse_x) * sensitivity,
                  float(mouse_y) * sensitivity *
                      (REXCVAR_GET(kk_mouse_invert_y) ? -1.f : 1.f));
      if (photo) camera.speed = photo_speed.load();
      else camera.Wheel(float(wheel));
      PadInput pad;
      // Use the first pad with an active control; never add duplicate inputs
      // when the title polls more than one connected user.
      for (const auto &p : pads)
        if (p.forward || p.side || p.vertical || p.look_x || p.look_y ||
            p.fast || p.slow) {
          pad = p;
          break;
        }
      const float turn = std::clamp(REXCVAR_GET(kk_camera_sensitivity), 10, 400) /
                         100.f * 1.8f * std::clamp(real_dt, 0.f, .1f);
      camera.Look(pad.look_x * turn, pad.look_y * turn);
      camera.Move(
          float(keys['W']) - keys['S'] + pad.forward,
          float(keys['D']) - keys['A'] + pad.side,
          float(keys['E'] || keys[0x20]) -
              float(keys['Q'] || keys[0x11] || keys[0xA2] || keys[0xA3]) + pad.vertical,
          keys[0x10] || keys[0xA0] || keys[0xA1] || pad.fast,
          keys[0x12] || keys[0xA4] || keys[0xA5] || pad.slow, real_dt);
    }
    if (photo) camera.position = ClampToRadius(camera.position, photo_center, photo_radius);
    mouse_x = mouse_y = wheel = 0;
#if defined(KK_DEV_TOOLS)
    static auto last_trace = Clock::time_point{};
    if (std::getenv("KK_DEV_FREECAM_TEST") &&
        Clock::now() - last_trace > std::chrono::milliseconds(200)) {
      last_trace = Clock::now();
      const Vec original = ReadVec(base, view + 48);
      const Vec player = ReadVec(base, 0x82D64250);
      REXLOG_INFO("KK freecam test: sample stage={} camera={:.5f},{:.5f},{:.5f} "
                  "yaw={:.5f} pitch={:.5f} player={:.5f},{:.5f},{:.5f} "
                  "game_camera={:.5f},{:.5f},{:.5f} polls={} leaks={}",
                  trace_stage.load(), camera.position.x, camera.position.y,
                  camera.position.z, camera.yaw, camera.pitch,
                  player.x, player.y, player.z, original.x, original.y, original.z,
                  pad_polls.load(), pad_leaks.load());
    }
#endif
  }
  std::memcpy(saved.data(), base + view, saved.size());
  const float roll = photo ? photo_roll.load() : 0.f;
  WriteVec(base, view, RollAxis(camera.Right(), camera.Up(), roll, true));
  WriteVec(base, view + 16, camera.Forward());
  WriteVec(base, view + 32, RollAxis(camera.Right(), camera.Up(), roll, false));
  WriteVec(base, view + 48, camera.position);
  return true;
}
} // namespace kk::freecam

// The game's screen-space display list. Keep its lifetime/removal bookkeeping
// running while suppressing its leaf drawing routines, so F10 can restore
// persistent HUD elements immediately.
namespace {
thread_local bool hide_screen_list = false;
// One log line the first time a HUD draw is left out in a run (tests check kk_hide_hud with it).
void NoteHudHidden() {
  static std::atomic<bool> noted{false};
  if (!noted.exchange(true))
    REXLOG_INFO("KK freecam: game HUD hidden ({})", REXCVAR_GET(kk_hide_hud) ? "kk_hide_hud" : "F10");
}
}
REX_EXTERN(__imp__sub_827FA2B0);
REX_HOOK_RAW(sub_827FA2B0) {
  const bool old = hide_screen_list;
  hide_screen_list =
      old || (kk::freecam::HideGameHud() && uint32_t(ctx.lr) == 0x82873AB0);
  __imp__sub_827FA2B0(ctx, base);
  hide_screen_list = old;
}
#define KK_HUD_LEAF(addr)                                                      \
  REX_EXTERN(__imp__sub_##addr);                                               \
  REX_HOOK_RAW(sub_##addr) {                                                   \
    if (hide_screen_list) {                                                    \
      NoteHudHidden();                                                         \
      ctx.r3.u64 = 1;                                                          \
      return;                                                                  \
    }                                                                          \
    __imp__sub_##addr(ctx, base);                                              \
  }
KK_HUD_LEAF(827C57D0)
KK_HUD_LEAF(827CCB28)
KK_HUD_LEAF(82875BB8)
KK_HUD_LEAF(827C50B8)
KK_HUD_LEAF(82863938)
KK_HUD_LEAF(827C2518)
KK_HUD_LEAF(828750F0)
KK_HUD_LEAF(82863018)
KK_HUD_LEAF(827CC200)
// sub_82874C98 also draws the game's fades (the black fade in at a level start, cutscene ends), so Hide HUD and
// F10 keep it (DECISIONS 19 Oct; task 245: without it the untextured scene showed before its textures had
// loaded). Its HUD draws land in the HUD pass, which ultrawide.cpp tags "hide" and the renderer leaves out
// (textured and not full width). Photo mode, where that pass is not tagged, still leaves it out.
REX_EXTERN(__imp__sub_82874C98);
REX_HOOK_RAW(sub_82874C98) {
  if (hide_screen_list && kk::freecam::PhotoActive()) {
    NoteHudHidden();
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_82874C98(ctx, base);
}
KK_HUD_LEAF(827CBC70)
KK_HUD_LEAF(827C8C20)
KK_HUD_LEAF(828745D0)
KK_HUD_LEAF(82873DF8)
KK_HUD_LEAF(827C7438)
KK_HUD_LEAF(827C38E0)
KK_HUD_LEAF(827C6B30)
KK_HUD_LEAF(827C6078)
KK_HUD_LEAF(827C40E8)
#undef KK_HUD_LEAF
REX_EXTERN(__imp__sub_827C6660);
REX_HOOK_RAW(sub_827C6660) {
  // First-person weapon pass: hidden by F10 (screenshots) and the free camera, not by kk_hide_hud.
  if ((hide_screen_list && kk::freecam::HideHud()) || kk::freecam::Active())
    return;
  __imp__sub_827C6660(ctx, base);
}
