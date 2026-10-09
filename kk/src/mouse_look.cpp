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

#include "mouse_look.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>

#include <rex/cvar.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

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
std::atomic<bool> g_on{false};  // Keyboard & mouse with the mouse camera, as of the last frame

std::mutex g_mutex;
double g_dx = 0, g_dy = 0;  // counts since CM_Cam last read the stick

// This frame (game thread): the turns still to give CM_Cam, in radians
// (yaw positive right, pitch positive up), and whether the stick was nudged.
double g_yaw = 0, g_pitch = 0;
bool g_nudged_x = false, g_nudged_y = false;
// Which sign of CM_Cam's angle turns right / up, learned from a nudge (0 until then).
int g_yaw_sign = 0, g_pitch_sign = 0;
Clock::time_point g_last_read{};

class Listener : public rex::ui::WindowInputListener {
 public:
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    // The movement is only reported while the pointer is locked (mouse look).
    if (!g_on.load(std::memory_order_relaxed) || (e.dx() == 0.0f && e.dy() == 0.0f)) return;
    std::lock_guard lock(g_mutex);
    g_dx += e.dx();
    g_dy += e.dy();
  }
};
Listener g_listener;

bool MouseCamera() {
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

#if defined(KK_DEV_TOOLS)
// Developer test aid: KK_DEV_MOUSE="<counts/s across>,<counts/s down>" moves
// the mouse that fast whenever the mouse camera is on.
void AddDevMouse(double seconds) {
  static const auto speed = [] {
    std::pair<double, double> s{0, 0};
    if (const char* v = std::getenv("KK_DEV_MOUSE"); v && *v) {
      s.first = std::atof(v);
      if (const char* comma = std::strchr(v, ',')) s.second = std::atof(comma + 1);
    }
    return s;
  }();
  g_dx += speed.first * seconds * Density();
  g_dy += speed.second * seconds * Density();
}
#endif

}  // namespace

void Install(rex::ui::Window* window) {
  if (!window || g_window) return;
  g_window = window;
  window->AddInputListener(&g_listener, 1000);  // ahead of the runtime's, which still gets every event
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
  g_yaw = g_pitch = 0;
  g_nudged_x = g_nudged_y = false;
  // Movement that piled up while the camera wasn't reading (a cutscene, a
  // menu, Kong's chapters) is dropped rather than turned all at once.
  if (!on || since > 0.25) return;

  const double scale = kRadiansPerCount * std::clamp(REXCVAR_GET(kk_mouse_sensitivity), 0.05, 10.0) / Density();
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
