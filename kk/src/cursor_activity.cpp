#include "cursor_activity.h"
#include "freecam.h"
#include "debug_overlay.h"

#include <cmath>
#include <map>
#include <mutex>
#if defined(KK_DEV_TOOLS)
#include <chrono>
#include <cstdlib>
#include <rex/logging.h>
#endif
#include <SDL3/SDL.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context.h>

namespace kk {
namespace {
class Cursor;
// Stable timer state: queued UI callbacks can outlive a dialog during shutdown.
struct PollState {
  std::mutex mutex;
  Cursor* owner = nullptr;
  rex::ui::WindowedAppContext* context = nullptr;
  bool queued = false;
} g_poll;
class Cursor final : public rex::ui::ImGuiDialog, public rex::ui::WindowInputListener {
 public:
  Cursor(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window) : ImGuiDialog(drawer), window_(window) {
    // Observe before F8 consumes mouse events. Never consume an event ourselves.
    window_->AddInputListener(this, 1300);
    ready_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    {
      std::lock_guard lock(g_poll.mutex);
      g_poll.owner = this; g_poll.context = &window_->app_context();
    }
    timer_ = SDL_AddTimer(16, &Cursor::Timer, nullptr);
  }
  ~Cursor() override {
    {
      std::lock_guard lock(g_poll.mutex);
      g_poll.owner = nullptr; g_poll.context = nullptr;
    }
    if (timer_) SDL_RemoveTimer(timer_);
    window_->RemoveInputListener(this);
    for (auto& [id, pad] : pads_) SDL_CloseGamepad(pad);
    if (ready_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  }
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    if (e.dx() || e.dy() || (have_mouse_ && (e.x() != x_ || e.y() != y_))) Mouse();
    x_ = e.x(); y_ = e.y(); have_mouse_ = true;
  }
  void OnMouseDown(rex::ui::MouseEvent&) override { Mouse(); }
  void OnMouseWheel(rex::ui::MouseEvent&) override { Mouse(); }
  bool NeedsContinuousPaint() override { return false; }

 protected:
  void OnDraw(ImGuiIO&) override { Apply(); }

 private:
  static Uint32 SDLCALL Timer(void*, SDL_TimerID, Uint32 interval) {
    std::lock_guard lock(g_poll.mutex);
    if (!g_poll.context) return 0;
    if (!g_poll.queued) {
      g_poll.queued = true;
      g_poll.context->CallInUIThreadDeferred([] {
        Cursor* owner;
        {
          std::lock_guard lock(g_poll.mutex);
          g_poll.queued = false; owner = g_poll.owner;
        }
        if (owner) owner->Tick();
      });
    }
    return interval;
  }
  void Tick() {
    // Poll on the UI thread even when painting is idle. The timer only queues
    // this small observer; it never requests an ImGui/presenter redraw.
    // Read physical SDL devices, not the game's merged XInput state: keyboard
    // and mouse emulation must never be mistaken for controller activity.
    if (ready_) {
      SDL_UpdateGamepads();
      int count = 0;
      SDL_JoystickID* ids = SDL_GetGamepads(&count);
      for (int i = 0; i < count; ++i) {
        auto& pad = pads_[ids[i]];
        if (!pad) pad = SDL_OpenGamepad(ids[i]);
        if (!pad) continue;
        CursorActivity::Pad state;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
          if (SDL_GetGamepadButton(pad, SDL_GamepadButton(b))) state.buttons |= uint32_t(1) << b;
        for (int a = 0; a < 6; ++a) {
          const int v = SDL_GetGamepadAxis(pad, SDL_GamepadAxis(a));
          // Ignore stick drift and small fluctuations of a held analogue input.
          const int deadzone = a < 4 ? 8000 : 4000;
          state.axes[a] = std::abs(v) > deadzone ? v / 2048 : 0;
        }
        if (!mouse_this_frame_) activity_.Controller(previous_[ids[i]], state);
        previous_[ids[i]] = state;
      }
      SDL_free(ids);
#if defined(_WIN32)
      // Our static SDL is separate from the runtime's. Both launcher input and
      // this observer read device state, so do not accumulate unused pad events.
      SDL_FlushEvents(SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED);
#endif
      for (auto it = pads_.begin(); it != pads_.end();) {
        if (it->second && SDL_GamepadConnected(it->second)) { ++it; continue; }
        if (it->second) SDL_CloseGamepad(it->second);
        previous_.erase(it->first);
        it = pads_.erase(it);
      }
    }
    Apply();
    mouse_this_frame_ = false;
#if defined(KK_DEV_TOOLS)
    TestTick();
#endif
  }

#if defined(KK_DEV_TOOLS)
  void TestTick() {
    if (!std::getenv("KK_DEV_CURSOR_TEST") || test_stage_ >= 8) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < test_next_) return;
    test_next_ = now + std::chrono::milliseconds(300);
    auto check = [&](bool mouse,const char* description) {
      const bool pass = (window_->GetCursorVisibility() == rex::ui::Window::CursorVisibility::kVisible) == mouse;
      REXLOG_INFO("KK cursor test: {} {}",pass ? "PASS" : "FAIL",description);
    };
    switch (test_stage_++) {
      case 0: {
        Mouse();
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = 6; desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.axis_mask = 0x3f; desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT)-1;
        desc.name = "Task 123 cursor test";
        test_id_ = SDL_AttachVirtualJoystick(&desc);
        test_pad_ = SDL_OpenJoystick(test_id_);
        if (!test_pad_) { REXLOG_INFO("KK cursor test: FAIL virtual pad: {}",SDL_GetError()); test_stage_=8; return; }
        SDL_SetJoystickVirtualAxis(test_pad_,4,-32768); SDL_SetJoystickVirtualAxis(test_pad_,5,-32768);
        break;
      }
      case 1: SDL_SetJoystickVirtualButton(test_pad_,0,true); break;
      case 2: check(false,"physical button hides pointer"); Mouse(); break;
      case 3: check(true,"mouse stays active while button held"); SDL_SetJoystickVirtualButton(test_pad_,0,false); break;
      case 4: check(true,"release keeps pointer visible"); SDL_SetJoystickVirtualAxis(test_pad_,2,24000); break;
      case 5: {
        check(false,"right stick hides pointer");
        rex::ui::MouseEvent event(window_,rex::ui::MouseEvent::Button::kNone,x_+1,y_,0,0,1,0);
        OnMouseMove(event); break;
      }
      case 6: check(true,"mouse motion restores pointer"); SDL_CloseJoystick(test_pad_); SDL_DetachVirtualJoystick(test_id_); break;
      case 7: check(true,"unplug keeps pointer visible"); REXLOG_INFO("KK cursor test: done"); break;
    }
  }
  int test_stage_ = 0;
  SDL_Joystick* test_pad_ = nullptr;
  SDL_JoystickID test_id_ = 0;
  std::chrono::steady_clock::time_point test_next_{};
#endif
  void Apply() {
    if ((freecam::Active() && !DebugOverlayOpen()) || freecam::HideHud()) {
      window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
      return;
    }
    window_->SetCursorVisibility(activity_.mouse ? rex::ui::Window::CursorVisibility::kVisible
                                               : rex::ui::Window::CursorVisibility::kHidden);
  }
  void Mouse() { activity_.Mouse(); mouse_this_frame_ = true; Apply(); }
  rex::ui::Window* window_;
  SDL_TimerID timer_ = 0;
  CursorActivity activity_;
  std::map<SDL_JoystickID, SDL_Gamepad*> pads_;
  std::map<SDL_JoystickID, CursorActivity::Pad> previous_;
  bool ready_ = false, mouse_this_frame_ = false, have_mouse_ = false;
  int x_ = 0, y_ = 0;
};
}  // namespace
void CreateCursorActivity(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window) {
  if (window) new Cursor(drawer, window);  // owned with the other session dialogs
}
}  // namespace kk
