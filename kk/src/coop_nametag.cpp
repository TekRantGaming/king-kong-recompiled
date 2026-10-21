#include "coop_nametag.h"
#include "coop_pose.h"
#include "freecam.h"
#include "hfr.h"
#include "photo_mode.h"
#include "play_internal.h"
#include "settings.h"

#include <chrono>
#include <mutex>

namespace kk::fov { bool InMenus(); }
namespace kk::coop::nametag {
namespace {
double Now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
Matrix Read(const uint8_t* b, uint32_t a) {
  Matrix m{};
  for (unsigned i = 0; i < 16; ++i) m[i] = hfr::LoadF(b, a+4*i);
  return m;
}
bool InPlay() {
  return REXCVAR_GET(kk_coop_enabled) && play::SessionActive() && !play::CoverScreen() &&
      !play::StartLevelLoading() && !play::PauseMenuOpen() && !play::GamePausePageUp() &&
      !photo::Active() && !freecam::Active() && !kk::fov::InMenus();
}
struct Normal { uint32_t actor = 0, matrix = 0, level = 0; double time = 0; };
std::mutex mutex;
Normal normal;
std::optional<Camera> camera;
const char* status = "waiting-normal-camera";
// The scene setup and nested projection callback run on the same guest thread.
// Do not inspect the scratch projection later: weapon/HUD passes overwrite it.
thread_local std::optional<Camera> pending;
thread_local bool have_projection = false;
}
void NormalCamera(const uint8_t* b, uint32_t actor, uint32_t caller) {
  if (!REXCVAR_GET(kk_coop_enabled) || caller != 0x824715FC) return;
  const auto script = hfr::Load32(b, 0x82CCBF64);
  const auto cm = PoseAddress(script, 68) ? hfr::Load32(b, script+64) : 0;
  const auto hero = hfr::Load32(b, 0x82CCBF7C);
  const auto matrix = PoseAddress(actor, 28) ? hfr::Load32(b, actor+12) : 0;
  // C09's exact CM_Cam call orients the camera, its next call orients Jack.
  // This evidence is refreshed only by the normal first-person commit, not a cinematic.
  Normal next;
  if (InPlay() && PoseAddress(cm, 232) && hfr::Load32(b, cm) == hero && actor != hero &&
      actor == hfr::Load32(b, 0x82CCBF6C) && PoseAddress(matrix, 64)) {
    const auto pose = ReadJackPose([&](uint32_t a) { return hfr::Load32(b,a); },
        [&](uint32_t a) { return hfr::Load16(b,a); }, [&](uint32_t a) { return hfr::LoadF(b,a); }, Now());
    if (pose.pose) next = {actor, matrix, pose.pose->level, Now()};
  }
  std::lock_guard lock(mutex); normal = next;
}
std::optional<float> ViewPitch(const uint8_t* b, uint32_t level) {
  Normal n;
  { std::lock_guard lock(mutex); n = normal; }
  if (!n.time || Now()-n.time > .15 || n.level != level || !PoseAddress(n.actor, 28) ||
      hfr::Load32(b,n.actor+12) != n.matrix || !PoseAddress(n.matrix,64)) return {};
  const float x=-hfr::LoadF(b,n.matrix+16), y=-hfr::LoadF(b,n.matrix+20), z=-hfr::LoadF(b,n.matrix+24);
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return {};
  return std::atan2(z,std::hypot(x,y));
}
bool BeginView(const uint8_t* b, uint32_t view, uint32_t caller) {
  if (!REXCVAR_GET(kk_coop_enabled) || caller != 0x8287390C) return false;
  pending.reset(); have_projection = false;
  Normal n;
  { std::lock_guard lock(mutex); n = normal; }
  // Keep the preceding valid frame while its replacement is being assembled;
  // a failed setup clears it immediately. Avoid UI-thread flicker during setup.
  auto reject = [](const char* why) {
    std::lock_guard lock(mutex); camera.reset(); status = why; return false;
  };
  if (!InPlay()) return reject("not-in-play");
  if (Now()-n.time > .15 || !n.time) return reject("normal-camera-expired");
  if (!PoseAddress(view,192) || !PoseAddress(n.actor,28) ||
      hfr::Load32(b,n.actor+12) != n.matrix || !PoseAddress(n.matrix,64)) return reject("camera-span");
  // CM_Cam commits translation after orientation. Read its final matrix here,
  // rather than the earlier position at the normal orientation setter.
  const auto normal_matrix = Read(b,n.matrix);
  const auto g = hfr::Load32(b, 0x82CC97C0);
  if (!PoseAddress(g, 28) || hfr::Load32(b,g+4) || hfr::Load32(b,g+24) != n.level) return reject("level-or-load");
  // Scene views are copies of the camera transform. Match axes AND position;
  // a different scene/cinematic camera cannot inherit the normal-camera stamp.
  for (unsigned row = 0; row < 4; ++row) for (unsigned col = 0; col < 3; ++col) {
    const auto i = row*4+col;
    const float f = hfr::LoadF(b,view+4*i);
    if (!std::isfinite(f) || !std::isfinite(normal_matrix[i]) ||
        std::abs(f-normal_matrix[i]) > .02f) return reject("different-scene-camera");
  }
  pending = Camera{}; pending->level = n.level;
  return true;
}
void ProjectionPush(const uint8_t* b, uint32_t matrix) {
  if (!pending || !PoseAddress(matrix,64)) return;
  pending->projection = Read(b,matrix);
  have_projection = true;
}
void EndView(const uint8_t* b) {
  if (!pending) return;
  // 82872DB8: GDI=*82CCAC30, 827E64C8 converts camera axes at GDI+352;
  // 827E5BB8(GDI+208) transposes/inverts them into GDI+432, then copies
  // that exact world-to-view transform to GDI+1040 (generated .0/.27/.40).
  const auto gdi = hfr::Load32(b,0x82CCAC30);
  std::optional<Camera> next;
  if (have_projection && PoseAddress(gdi, 1104) && InPlay()) {
    pending->view = Read(b,gdi+432); pending->time = Now();
    if (Valid(*pending)) next = pending;
  }
  { std::lock_guard lock(mutex); camera = next; status = next ? "scene-valid" : "no-perspective"; }
  pending.reset(); have_projection = false;
}
std::optional<Camera> Snapshot() { std::lock_guard lock(mutex); return camera; }
const char* Status() { std::lock_guard lock(mutex); return status; }
}  // namespace kk::coop::nametag
