#include "freecam.h"
#include "debug_overlay.h"
#include "freecam_math.h"
#include "play_internal.h"
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

namespace kk::freecam {
namespace {
using Clock = std::chrono::steady_clock;
std::atomic<bool> active{false}, hidden{false};
std::atomic<uint32_t> generation{0};
std::mutex input_mutex;
bool keys[256]{};
double mouse_x = 0, mouse_y = 0, wheel = 0;
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
void ClearInput() {
  std::lock_guard lock(input_mutex);
  std::memset(keys, 0, sizeof(keys));
  mouse_x = mouse_y = wheel = 0;
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
    if (std::getenv("KK_DEV_FREECAM_TEST") && test_stage < 12)
      return true;
#endif
    return active.load();
  }
  void OnLostFocus(rex::ui::UISetupEvent &) override { ClearInput(); }
  void OnKeyDown(rex::ui::KeyEvent &e) override { Key(e, true); }
  void OnKeyUp(rex::ui::KeyEvent &e) override { Key(e, false); }
  void OnMouseMove(rex::ui::MouseEvent &e) override {
    if (!active || DebugOverlayOpen() || !InPlay())
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
    if (!active || DebugOverlayOpen() || !InPlay())
      return;
    std::lock_guard lock(input_mutex);
    wheel += double(e.scroll_y()) / rex::ui::MouseEvent::kScrollPerDetent;
    e.set_handled(true);
  }
  void OnMouseDown(rex::ui::MouseEvent &e) override {
    if (active && !DebugOverlayOpen())
      e.set_handled(true);
  }
  void OnMouseUp(rex::ui::MouseEvent &e) override {
    if (active && !DebugOverlayOpen())
      e.set_handled(true);
  }

protected:
  void OnDraw(ImGuiIO &) override {
#if defined(KK_DEV_TOOLS)
    TestTick();
#endif
    const bool want =
        active && InPlay() && !DebugOverlayOpen() && window->HasFocus();
    if (want != relative) {
      relative = want;
      const bool normal = rex::cvar::GetFlagByName("mnk_mode") == "true" &&
                          rex::cvar::GetFlagByName("mnk_mouse") == "true" &&
                          InPlay() && !DebugOverlayOpen() && window->HasFocus();
      window->SetRelativeMouseMode(want || normal);
    }
  }

private:
#if defined(KK_DEV_TOOLS)
  void TestTick() {
    if (!std::getenv("KK_DEV_FREECAM_TEST") || !InPlay())
      return;
    if (test_start == Clock::time_point{})
      test_start = Clock::now();
    const double t =
        std::chrono::duration<double>(Clock::now() - test_start).count();
    constexpr double at[] = {4, 5, 8, 9, 11, 14, 15, 17, 18, 20, 22, 24};
    if (test_stage >= std::size(at) || t < at[test_stage])
      return;
    auto key = [&](unsigned k, bool down) {
      rex::ui::KeyEvent e(window, rex::ui::VirtualKey(k), 1, false, false,
                          false, false, false);
      Key(e, down);
    };
    auto tap = [&](unsigned k) {
      key(k, true);
      key(k, false);
    };
    auto wheel_event = [&](int delta) {
      rex::ui::MouseEvent e(window, rex::ui::MouseEvent::Button::kNone, 0, 0, 0,
                            delta);
      OnMouseWheel(e);
    };
    switch (test_stage) {
    case 0:
      tap(0x78);
      break;
    case 1: {
      key('W', true);
      key('D', true);
      key('E', true);
      rex::ui::MouseEvent e(window, rex::ui::MouseEvent::Button::kNone, 0, 0, 0,
                            0, 140, -60);
      OnMouseMove(e);
      wheel_event(240);
      break;
    }
    case 2:
      key('W', false);
      key('D', false);
      key('E', false);
      tap(0x79);
      break;
    case 3:
      key('W', true);
      key(0x10, true);
      wheel_event(1440);
      break;
    case 4:
      key('W', false);
      key(0x10, false);
      break;
    case 5:
      tap(0x78);
      break;
    case 6:
      tap(0x79);
      break;
    case 7:
      tap(0x78);
      break;
    case 8:
      key('Q', true);
      key('A', true);
      key(0x12, true);
      wheel_event(-1440);
      break;
    case 9:
      key('Q', false);
      key('A', false);
      key(0x12, false);
      break;
    case 10:
      tap(0x78);
      break;
    case 11:
      REXLOG_INFO("KK freecam test: DONE active={} hidden={}", active.load(),
                  hidden.load());
      break;
    }
    REXLOG_INFO("KK freecam test: stage {} at {:.3f}", test_stage++, t);
  }
  unsigned test_stage = 0;
  Clock::time_point test_start;
#endif
  void Key(rex::ui::KeyEvent &e, bool down) {
    const unsigned k = unsigned(e.virtual_key());
    if (k == 0x78 || k == 0x79) {
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
  new Controls(d, w);
}
bool Active() { return active.load(); }
bool HideHud() { return hidden.load() && InPlay(); }
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
      camera.Wheel(float(wheel));
      camera.Move(
          float(keys['W']) - keys['S'], float(keys['D']) - keys['A'],
          float(keys['E'] || keys[0x20]) -
              float(keys['Q'] || keys[0x11] || keys[0xA2] || keys[0xA3]),
          keys[0x10] || keys[0xA0] || keys[0xA1],
          keys[0x12] || keys[0xA4] || keys[0xA5], real_dt);
    }
    mouse_x = mouse_y = wheel = 0;
#if defined(KK_DEV_TOOLS)
    static auto last_trace = Clock::time_point{};
    if (std::getenv("KK_DEV_FREECAM_TEST") &&
        Clock::now() - last_trace > std::chrono::seconds(1)) {
      last_trace = Clock::now();
      const Vec original = ReadVec(base, view + 48);
      REXLOG_INFO("KK freecam test: position {:.3f} {:.3f} {:.3f}; game camera "
                  "{:.3f} {:.3f} {:.3f}; distance {:.2f}; speed {:.2f}",
                  camera.position.x, camera.position.y, camera.position.z,
                  original.x, original.y, original.z,
                  Length(camera.position + original * -1), camera.speed);
    }
#endif
  }
  std::memcpy(saved.data(), base + view, saved.size());
  WriteVec(base, view, camera.Right());
  WriteVec(base, view + 16, camera.Forward());
  WriteVec(base, view + 32, camera.Up());
  WriteVec(base, view + 48, camera.position);
  return true;
}
} // namespace kk::freecam

// The game's screen-space display list. Keep its lifetime/removal bookkeeping
// running while suppressing its leaf drawing routines, so F10 can restore
// persistent HUD elements immediately.
namespace {
thread_local bool hide_screen_list = false;
}
REX_EXTERN(__imp__sub_827FA2B0);
REX_HOOK_RAW(sub_827FA2B0) {
  const bool old = hide_screen_list;
  hide_screen_list =
      old || (kk::freecam::HideHud() && uint32_t(ctx.lr) == 0x82873AB0);
  __imp__sub_827FA2B0(ctx, base);
  hide_screen_list = old;
}
#define KK_HUD_LEAF(addr)                                                      \
  REX_EXTERN(__imp__sub_##addr);                                               \
  REX_HOOK_RAW(sub_##addr) {                                                   \
    if (hide_screen_list) {                                                    \
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
KK_HUD_LEAF(82874C98)
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
  if (hide_screen_list || kk::freecam::Active())
    return; // first-person weapon pass
  __imp__sub_827C6660(ctx, base);
}
