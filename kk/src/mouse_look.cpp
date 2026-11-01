// Keyboard & mouse: the mouse turns Jack's camera directly.
//
// ReXGlue turns mouse movement into a right-stick push, and Jack's camera
// (CM_Cam, sub_8246F180) treats that like any stick: it drops a 15% deadzone,
// squares the yaw and cubes the pitch, builds the turn speed up while the
// stick is held and caps it at a full push. So small and medium movements did
// nothing and fast ones hit the cap. Instead the window's mouse movement is
// added up here, the runtime's own mouse-to-stick part is switched off
// (mnk_sensitivity 0, settings.cpp), and each frame CM_Cam's turns (its calls
// to sub_82711950 for yaw and sub_82712300 for pitch, input_remap.cpp) get the
// angle the mouse moved: a fixed angle per count at any speed and frame rate.
// CM_Cam only makes those calls for an axis whose stick is past its deadzone,
// so while the mouse moves, its stick read is nudged just past it; the small
// turn the game works out from that is replaced by the mouse's.
//
// The movement comes from SDL as it is sent (on Windows on SDL's raw input
// thread), not from the window's events. The window thread also paints, so it
// passed the movement on in bursts: with an even frame rate, one frame in twenty
// got none and the next got two frames' worth, while the camera has no smoothing
// to hide that. A stick is read on the game thread and never showed it (issue 47).

#include "mouse_look.h"
#include "benchmark.h"
#include "debug_overlay.h"
#include "freecam.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

#if defined(KK_DEV_TOOLS) && defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/window_sdl.h>
#include <rex/ui/windowed_app_context_sdl.h>

#include "settings.h"

namespace kk::mouse_look {
namespace {

using Clock = std::chrono::steady_clock;

constexpr float kCamDeadzone = 0.15f;      // CM_Cam's own, per axis
constexpr float kNudge = kCamDeadzone + 0.02f;
// Radians per count at sensitivity 1 (0.066 degrees, the Source engine's yaw
// at sensitivity 3: about 34 cm of a 1600 dpi mouse for a full turn).
constexpr double kRadiansPerCount = 0.066 * 3.14159265358979 / 180.0;

rex::ui::Window* g_window = nullptr;
rex::ui::WindowSDL* g_sdl_window = nullptr;
std::atomic<bool> g_on{false};  // Keyboard & mouse with the mouse camera, as of the last frame

std::mutex g_mutex;
double g_dx = 0, g_dy = 0;  // counts since CM_Cam last read the stick
double g_tutorial_dy = 0;  // Venture's separate vertical-look intent read

// This frame (game thread): the turns still to give CM_Cam, in radians
// (yaw positive right, pitch positive up), and whether the stick was nudged.
double g_yaw = 0, g_pitch = 0;
bool g_nudged_x = false, g_nudged_y = false;
// Which sign of CM_Cam's angle turns right / up, learned from a nudge (0 until then).
int g_yaw_sign = 0, g_pitch_sign = 0;
Clock::time_point g_last_read{};

void OnMouseMotion(float dx, float dy) {
  // The movement is only reported while the pointer is locked (mouse look).
  if (!g_on.load(std::memory_order_relaxed) || (dx == 0.0f && dy == 0.0f)) return;
#if defined(KK_DEV_TOOLS)
  if (static std::atomic<bool> logged{false}; !logged.exchange(true)) REXLOG_INFO("KK mouse: the first movement came on this thread");
#endif
  std::lock_guard lock(g_mutex);
  g_dx += dx;
  g_dy += dy;
  g_tutorial_dy += dy;
}

#if defined(KK_DEV_TOOLS)
// The window's events, the old way in: the log compares them with OnMouseMotion's (DevCompare).
double g_dev_window_dx = 0;
class DevListener : public rex::ui::WindowInputListener {
 public:
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    std::lock_guard lock(g_mutex);
    g_dev_window_dx += e.dx();
  }
};
DevListener g_dev_listener;
#endif

bool MouseCamera() {
  if (kk::freecam::Active()) return false;
  if (kk::benchmark::Active()) return false;
  if (kk::DebugOverlayOpen()) return false;
#if defined(KK_DEV_TOOLS)
  // KK_DEV_MOUSE (below) tests mouse look without Keyboard & mouse, so the
  // runtime doesn't lock the pointer to the window.
  static const bool dev_mouse = std::getenv("KK_DEV_MOUSE") != nullptr;
  if (dev_mouse) return true;
#endif
  return rex::cvar::GetFlagByName("mnk_mode") == "true" && rex::cvar::GetFlagByName("mnk_mouse") == "true";
}

// The window reports the movement in pixels (counts times the display's pixel
// density); counts are what a sensitivity means.
double Density() {
  if (!g_window || !g_window->GetMediumDpi()) return 1.0;
  const double d = double(g_window->GetDpi()) / g_window->GetMediumDpi();
  return d > 0.25 ? d : 1.0;
}

double PixelDensity() { return g_sdl_window ? g_sdl_window->GetPixelDensity() : 1.0; }

#if defined(KK_DEV_TOOLS)
// Developer test aid: KK_DEV_MOUSE="<counts/s across>,<counts/s down>" moves
// the mouse that fast whenever the mouse camera is on.
const std::pair<double,double>& DevMouseSpeed() {
  static const auto speed = [] {
    std::pair<double, double> s{0, 0};
    if (const char* v = std::getenv("KK_DEV_MOUSE"); v && *v) {
      s.first = std::atof(v);
      if (const char* comma = std::strchr(v, ',')) s.second = std::atof(comma + 1);
    }
    return s;
  }();
  return speed;
}
void AddDevMouse(double seconds) {
  const auto& speed=DevMouseSpeed();
  g_dx += speed.first * seconds * Density();
  g_dy += speed.second * seconds * Density();
}

#if defined(_WIN32)
// Developer test aid: KK_DEV_MOUSE_EVENTS=1 posts a mouse move to the window
// about every millisecond while the mouse camera is on, so a background window
// gets movement. It comes through the window thread, as the old way in did.
void StartDevEvents() {
  if (!std::getenv("KK_DEV_MOUSE_EVENTS")) return;
  std::thread([] {
    const HWND window = HWND(g_window->GetNativeWindowHandle());
    for (int x = 0;; x = (x + 1) % 2) {
      Sleep(1);
      if (g_on.load(std::memory_order_relaxed)) PostMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(100 + x, 100));
    }
  }).detach();
}
#endif

// Every 600 frames with mouse movement the log says how many frames the window's
// events gave none or over one and a half times what OnMouseMotion gave, and how
// many OnMouseMotion gave none, for comparing the two with a real mouse, and how
// many frames' turns CM_Cam never took (a cutscene, or the camera not reading them).
void DevCompare(double motion_dx) {
  double window_dx;
  {
    std::lock_guard lock(g_mutex);
    window_dx = g_dev_window_dx;
    g_dev_window_dx = 0;
  }
  static int frames = 0, window_none = 0, window_high = 0, motion_none = 0, not_taken = 0;
  if (motion_dx == 0 && window_dx == 0) return;
  if (g_yaw != 0) ++not_taken;
  if (motion_dx == 0) ++motion_none;
  if (window_dx == 0) ++window_none;
  else if (std::fabs(window_dx) > std::fabs(motion_dx) * 1.5) ++window_high;
  if (++frames == 600) {
    REXLOG_INFO("KK mouse compare: 600 frames: window events none {} too many {}; direct none {}; turns not taken {}",
                window_none, window_high, motion_none, not_taken);
    frames = window_none = window_high = motion_none = not_taken = 0;
  }
}
#endif

}  // namespace

void Install(rex::ui::Window* window) {
  if (!window || g_window) return;
  g_window = window;
  g_sdl_window = dynamic_cast<rex::ui::WindowSDL*>(window);
  rex::ui::SDLWindowedAppContext::SetMouseMotionWatch(OnMouseMotion);
#if defined(KK_DEV_TOOLS)
  window->AddInputListener(&g_dev_listener, 1000);
#if defined(_WIN32)
  StartDevEvents();
#endif
#endif
}

void OnCameraStick(float& x, float& y) {
  const bool on = MouseCamera();
  g_on.store(on, std::memory_order_relaxed);
  const auto now = Clock::now();
  const double since = std::chrono::duration<double>(now - g_last_read).count();
  g_last_read = now;
  double dx, dy;
  {
    std::lock_guard lock(g_mutex);
#if defined(KK_DEV_TOOLS)
    if (on && since < 0.25) AddDevMouse(since);
#endif
    dx = g_dx;
    dy = g_dy;
    g_dx = g_dy = 0;
  }
#if defined(KK_DEV_TOOLS)
  if (on) DevCompare(dx);
#endif
  g_yaw = g_pitch = 0;
  g_nudged_x = g_nudged_y = false;
  // Movement that piled up while the camera wasn't reading (a cutscene, a
  // menu, Kong's chapters) is dropped rather than turned all at once.
  if (!on || since > 0.25) return;

  const double scale =
      kRadiansPerCount * std::clamp(REXCVAR_GET(kk_mouse_sensitivity), 0.05, 10.0) * PixelDensity() / Density();
  g_yaw = dx * scale;
  g_pitch = -dy * scale * (REXCVAR_GET(kk_mouse_invert_y) ? -1.0 : 1.0);
  if (g_yaw != 0 && std::fabs(x) <= kCamDeadzone) {
    x = g_yaw > 0 ? kNudge : -kNudge;
    g_nudged_x = true;
  }
  if (g_pitch != 0 && std::fabs(y) <= kCamDeadzone) {
    y = g_pitch > 0 ? kNudge : -kNudge;
    g_nudged_y = true;
  }
}

void OnVentureTutorialStick(float& y) {
  const bool on=MouseCamera();
  g_on.store(on,std::memory_order_relaxed);
  static Clock::time_point last{};
  const auto now=Clock::now();
  const double since=std::chrono::duration<double>(now-last).count();
  last=now;
  double dy;
  {
    std::lock_guard lock(g_mutex);
    dy=g_tutorial_dy; g_tutorial_dy=0;
  }
  if (!on || since>.25) return;
#if defined(KK_DEV_TOOLS)
  dy+=DevMouseSpeed().second*since*Density();
#endif
  // The native tutorial requires >half-stick vertical intent to choose the
  // sighting direction. The runtime's deliberately tiny mouse-to-stick gain
  // cannot reach it, even though CM_Cam already receives direct mouse turns.
  if (dy!=0 && std::fabs(y)<.5f)
    y=std::copysign(1.0f,float(-dy*(REXCVAR_GET(kk_mouse_invert_y) ? -1 : 1)));
}

namespace {
// The turn CM_Cam made from the stick (angle), with the mouse's (mouse, signed
// right/up) put in. A nudged axis takes the mouse's angle in the direction the
// game turned for the nudge; otherwise the stick is really pushed and the
// mouse's angle is added, once the sign is known.
double Combine(double angle, double& mouse, bool nudged, int& sign) {
  if (mouse == 0) return angle;
  double out = angle;
  if (nudged && angle != 0) {
    sign = (angle > 0) == (mouse > 0) ? 1 : -1;
    out = std::copysign(std::fabs(mouse), angle);
  } else if (sign != 0) {
    out = angle + sign * mouse;
  }
  mouse = 0;
  return out;
}
}  // namespace

double Yaw(double angle) { return Combine(angle, g_yaw, g_nudged_x, g_yaw_sign); }
double Pitch(double angle) { return Combine(angle, g_pitch, g_nudged_y, g_pitch_sign); }

}  // namespace kk::mouse_look
