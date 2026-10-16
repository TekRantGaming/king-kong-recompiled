// Developer-only: the camera at a chapter's start, to compare frame rates and the console (task 163;
// based on task 028's tracer on branch codex-camera). No guest state is written.
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>

namespace kk {
double DevSecondsSinceMenu();
void DevCameraGround(const uint8_t* base, uint32_t hero, uint32_t out, float speed_game, float speed_used);

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
