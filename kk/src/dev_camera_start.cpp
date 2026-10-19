// Developer-only: the camera at a chapter's start, to compare frame rates and the console (task 163;
// based on task 028's tracer on branch codex-camera). The tracing is read-only.
// KK_DEV_FACE_PARTNER=<seconds>[:<repeat seconds>] separately turns Jack and his
// normal first-person camera toward a fresh same-level co-op partner, with level
// pitch and no position writes. Due requests wait for a usable pose/camera.
// KK_DEV_FACE_ENEMY=<seconds>[:<repeat seconds>] takes precedence and aims at
// the nearest matched live enemy on the guest, including pitch at camera commit.
//
// KK_DEV_CAMERA_START=<s> with KK_DEV_SAMPLE_DIR writes, for the first <s> seconds on the dev script
// clock (from when the chapter's level has loaded; dev_tools.cpp DevSecondsSinceMenu):
//   camera_view.csv   one row per view copied for drawing (sub_827E64C8 called from sub_82872DB8):
//                     t, gt (0x82CCAE1C - 0x82CCC02C), dt (0x82C636E0), sixteen view floats,
//                     horizontal field of view (view +172), camera manager +228, hero x y z, hero
//                     previous x y z (+11656) and speed (+11668).
//   camera_ground.csv one row per CM_Cam ground-contact update (sub_824008D0 called from 82471560):
//                     t, gt, dt, hero state +11668 (the follow speed), coefficient 0x828E3A10, contact
//                     vector, camera right vector, camera forward vector (camera object 0x82CCBF6C), the
//                     speed the branch used (hfr_cam_ground.cpp; = the hero's speed when that fix is off).
// The ground branch turns the camera's forward z toward the contact plane by
// alpha = clamp(0.2 * speed * dt, 0, 1) (REPORT-028); what it leaves when the speed reaches 0 stays.

#if defined(KK_DEV_TOOLS)

#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>

#include "coop_pose.h"
#include "coop_runtime.h"
#include "coop_enemies_game.h"
#include "freecam.h"
#include "hfr.h"
#include "photo_mode.h"
#include "play_internal.h"

namespace kk {
double DevSecondsSinceMenu();
void DevCameraGround(const uint8_t* base, uint32_t hero, uint32_t out, float speed_game, float speed_used);

namespace {
// Separate from the dev script clock: rearmed after every actual level load.
struct FacePartner {
  using Clock = std::chrono::steady_clock;
  bool enabled = false, loaded = false, done = false, enemy = false, logged = false;
  double first = 0, repeat = 0, next = 0;
  Clock::time_point load_time{};

  FacePartner() {
    const char* value = std::getenv("KK_DEV_FACE_ENEMY");
    enemy = value && *value;
    if (!enemy) value = std::getenv("KK_DEV_FACE_PARTNER");
    if (!value || !*value) return;
    const std::string s(value);
    const size_t colon = s.find(':');
    auto number = [](const char* begin, const char* end, double& out) {
      const auto r = std::from_chars(begin, end, out);
      return begin != end && r.ec == std::errc{} && r.ptr == end && std::isfinite(out);
    };
    enabled = number(s.data(), s.data() + (colon == std::string::npos ? s.size() : colon), first) &&
              first >= 0;
    if (colon != std::string::npos)
      enabled = enabled && number(s.data() + colon + 1, s.data() + s.size(), repeat) && repeat > 0;
    if (!enabled) REXLOG_WARN("KK dev: invalid {} (expected seconds[:positive-repeat])", enemy ? "KK_DEV_FACE_ENEMY" : "KK_DEV_FACE_PARTNER");
  }
};
FacePartner& Face() { static FacePartner face; return face; }
}  // namespace

void DevFacePartnerLevelLoaded() {
  auto& f = Face();
  if (!f.enabled) return;
  f.loaded = true;
  f.done = false;
  f.logged = false;
  f.next = f.first;
  f.load_time = FacePartner::Clock::now();
}

// Called at CM_Cam's normal orientation commit, after ground tilt, before the
// original 82711328. Its next call (8247161C) uses this same forward vector to
// orient Jack horizontally. Both setters preserve position; no teleport occurs.
void DevFacePartnerCamera(PPCContext& ctx, uint8_t* base) {
  if (uint32_t(ctx.lr) != 0x824715FC) return;
  auto& f = Face();
  if (!f.enabled || !f.loaded || f.done) return;
  const double elapsed = std::chrono::duration<double>(FacePartner::Clock::now() - f.load_time).count();
  if (elapsed < f.next || play::StartLevelLoading() || photo::Active() || freecam::Active()) return;
  const auto snapshot = coop::PartnerBodySnapshot();
  if (!snapshot.enabled || !snapshot.fresh || snapshot.state != coop::SessionState::Connected ||
      !snapshot.local || (!f.enemy && !snapshot.partner)) return;
  using namespace hfr;
  const auto local = coop::ReadJackPose(
      [&](uint32_t a) { return Load32(base, a); },
      [&](uint32_t a) { return Load16(base, a); },
      [&](uint32_t a) { return LoadF(base, a); }, elapsed);
  if (!local.pose || (!f.enemy && local.pose->level != snapshot.partner->level) ||
      local.pose->level != snapshot.local->level) return;
  const uint32_t hero = Load32(base, 0x82CCBF7C);
  const uint32_t script = Load32(base, 0x82CCBF64);
  if (!coop::PoseAddress(script, 68)) return;
  const uint32_t cm = Load32(base, script + 64);
  const uint32_t camera = ctx.r3.u32, forward = ctx.r4.u32, up = ctx.r5.u32;
  // Bind to this live Jack camera, never a cinematic or another actor's setter.
  if (!coop::PoseAddress(cm, 232) || Load32(base, cm) != hero ||
      camera != Load32(base, 0x82CCBF6C) || camera == hero ||
      !coop::PoseAddress(camera, 28) || (Load32(base, camera) & 0x00400001u) ||
      !coop::PoseAddress(Load32(base, camera + 12), 64) ||
      forward != ctx.r1.u32 + 112 || !coop::PoseAddress(forward, 12) || up != cm + 208) return;
  const auto matrix = Load32(base, camera+12);
  const coop::ShotVector eye = f.enemy ? coop::ShotVector{LoadF(base, matrix+48), LoadF(base, matrix+52), LoadF(base, matrix+56)} :
      coop::ShotVector{local.pose->x, local.pose->y, local.pose->z};
  auto target = f.enemy ? coop::EnemyAimTarget(base, eye) :
      std::optional<coop::ShotVector>{coop::ShotVector{snapshot.partner->x, snapshot.partner->y, eye.z}};
  if (!target) return; // Due requests wait for a matched live enemy and normal Jack camera.
  const float dx = target->x - eye.x, dy = target->y - eye.y, dz = f.enemy ? target->z-eye.z : 0;
  const float horizontal = std::hypot(dx, dy);
  const float distance = std::hypot(horizontal, dz);
  // An overlapping partner has no useful yaw. Keep the due request pending.
  if (!std::isfinite(horizontal) || horizontal < .01f || !std::isfinite(distance)) return;
  StoreF(base, forward, dx / distance);
  StoreF(base, forward + 4, dy / distance);
  StoreF(base, forward + 8, dz / distance);
  StoreF(base, up, 0);
  StoreF(base, up + 4, 0);
  StoreF(base, up + 8, 1);
  StoreF(base, cm + 168, 0);  // CM_Cam's accumulated pitch in its constrained-view branch.
  // Repeated enemy aiming commits pitch with the same native orientation setter;
  // its following call turns Jack using the horizontal forward vector (.52:20959).
  if (!f.logged || !f.enemy || f.repeat >= 1)
    REXLOG_INFO("KK dev: face {} yaw={:.4f} pitch={:.4f} distance={:.3f}", f.enemy ? "enemy" : "partner",
        std::atan2(dy, dx), std::atan2(dz, horizontal), distance);
  f.logged = true;
  if (f.repeat > 0) {
    // Skip missed intervals, preserving the load-relative cadence without bursts.
    f.next = elapsed + (f.repeat - std::fmod(elapsed - f.first, f.repeat));
  } else {
    f.done = true;
  }
}

namespace {
uint32_t Word(const uint8_t* base, uint32_t a) {
  return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
}
float Float(const uint8_t* base, uint32_t a) { return std::bit_cast<float>(Word(base, a)); }
bool Valid(uint32_t a) { return a >= 0x40000000 && a < 0xA0000000 && !(a & 3); }

double Window() {
  static const double s = [] {
    const char* v = std::getenv("KK_DEV_CAMERA_START");
    return v && *v ? std::atof(v) : 0.0;
  }();
  return s;
}

FILE* Open(const char* name) {
  const char* dir = std::getenv("KK_DEV_SAMPLE_DIR");
  const std::string path = dir && *dir ? std::string(dir) + "/" + name : std::string(name);
  return std::fopen(path.c_str(), "w");
}

// Seconds on the script clock, or a negative value when not tracing.
double Now() {
  if (Window() <= 0) return -1;
  const double t = DevSecondsSinceMenu();
  if (t > Window()) {
    static bool done = false;
    if (!done) REXLOG_INFO("KK dev camera: done");
    done = true;
    return -1;
  }
  return t;
}

double GameTime(const uint8_t* base) { return double(Float(base, 0x82CCAE1C)) - Float(base, 0x82CCC02C); }
}  // namespace
}  // namespace kk

REX_EXTERN(__imp__sub_827E64C8);
REX_HOOK_RAW(sub_827E64C8) {
  const double t = kk::Now();
  if (t >= 0 && uint32_t(ctx.lr) == 0x82872DDC && kk::Valid(ctx.r4.u32)) {
    static FILE* f = kk::Open("camera_view.csv");
    if (f) {
      const uint32_t view = ctx.r4.u32;
      std::fprintf(f, "%.6f,%.6f,%.7g", t, kk::GameTime(base), kk::Float(base, 0x82C636E0));
      for (unsigned i = 0; i < 16; ++i) std::fprintf(f, ",%.9g", kk::Float(base, view + i * 4));
      std::fprintf(f, ",%.9g", kk::Float(base, view + 172));
      // Camera manager state ([[0x82CCBF64] +64], first word = hero): +228 (0 = ground branch on),
      // hero position, and the hero's script speed (+11668) and previous position (+11656).
      const uint32_t script = kk::Word(base, 0x82CCBF64);
      const uint32_t cm = kk::Valid(script) ? kk::Word(base, script + 64) : 0;
      const uint32_t hero = kk::Valid(cm) ? kk::Word(base, cm) : 0;
      const uint32_t hm = kk::Valid(hero) ? kk::Word(base, hero + 12) : 0;
      const uint32_t ext = kk::Valid(hero) ? kk::Word(base, hero + 24) : 0;
      const uint32_t ai = kk::Valid(ext) ? kk::Word(base, ext + 4) : 0;
      const uint32_t hs = kk::Valid(ai) ? kk::Word(base, ai + 64) : 0;
      std::fprintf(f, ",%.9g", kk::Valid(cm) ? kk::Float(base, cm + 228) : -1.0f);
      for (unsigned i = 0; i < 3; ++i) std::fprintf(f, ",%.9g", kk::Valid(hm) ? kk::Float(base, hm + 48 + i * 4) : 0.0f);
      for (unsigned i = 0; i < 4; ++i)
        std::fprintf(f, ",%.9g", kk::Valid(hs) ? kk::Float(base, hs + 11656 + i * 4) : 0.0f);
      std::fprintf(f, "\n");
      std::fflush(f);
    }
  }
  __imp__sub_827E64C8(ctx, base);
}

// Called by hfr_cam_ground.cpp after the ground branch's contact query (sub_824008D0 from 82471560).
void kk::DevCameraGround(const uint8_t* base, uint32_t hero, uint32_t out, float speed_game, float speed_used) {
  const double t = kk::Now();
  if (t < 0 || !kk::Valid(hero) || !kk::Valid(out)) return;
  const uint32_t camera_obj = kk::Word(base, 0x82CCBF6C);
  const uint32_t matrix = kk::Valid(camera_obj) ? kk::Word(base, camera_obj + 12) : 0;
  if (!kk::Valid(matrix)) return;
  static FILE* f = kk::Open("camera_ground.csv");
  if (!f) return;
  std::fprintf(f, "%.6f,%.6f,%.9g,%.9g,%.9g", t, kk::GameTime(base), kk::Float(base, 0x82C636E0), speed_game,
               kk::Float(base, 0x828E3A10));
  for (unsigned i = 0; i < 3; ++i) std::fprintf(f, ",%.9g", kk::Float(base, out + i * 4));
  for (unsigned i = 0; i < 3; ++i) std::fprintf(f, ",%.9g", kk::Float(base, matrix + i * 4));
  for (unsigned i = 0; i < 3; ++i) std::fprintf(f, ",%.9g", kk::Float(base, matrix + 16 + i * 4));
  std::fprintf(f, ",%.9g\n", speed_used);
  std::fflush(f);
}

#endif
