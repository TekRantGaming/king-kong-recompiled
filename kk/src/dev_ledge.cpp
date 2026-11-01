// Developer-only (issue 49): finds steps and ledges around Jack with the game's own ray cast and walks him at each
// one, so runs at different frame rates can be compared with no player.
//
// KK_DEV_LEDGE=<folder> (with KK_DEV_AUTOSKIP, whose pad carries the stick): from KK_DEV_LEDGE_START seconds on the
// KK_DEV_SCRIPT clock (default 3) floods the walkable ground from Jack's feet on a 0.4 unit grid, and keeps each step
// up of 0.12 to 0.95 units with a free run-up and head room over it: ledges.csv in the folder, at most
// KK_DEV_LEDGE_MAX (default 40), 1.5 units apart at least, high, middle and low steps in turn.
// KK_DEV_LEDGE_BAND=<low>,<high>: only steps of that height. KK_DEV_LEDGE_SPACING=<units>: the 1.5 above.
// KK_DEV_LEDGE_PATH=<csv of x,y,z>: Jack is put on each point in turn, KK_DEV_LEDGE_PATH_WAIT seconds apart (1), before
// the walks: the chapter's triggers then fire as on a played route (collision of a later area can need them).
// KK_DEV_CONTACTS=<x0>,<x1>,<y0>,<y1>: contacts.csv in the KK_DEV_LEDGE folder: every contact the game resolves for
// Jack (contact_resolve) while his new position is in that box.
// KK_DEV_LEDGE_RUNUP=<units>: how far before the step a walk starts (1.2).
// KK_DEV_LEDGE_TIME=<seconds>: the 3 s above. KK_DEV_LEDGE_DROP=<units>: the flood goes down drops up to this (3.5).
// KK_DEV_SPOTS=<folder>: for a person playing to a spot: spots.csv gets one row a frame while Jack is played (seconds,
// frame time, level, x, y, z, forward x, y, marks so far); the End key adds a mark.
// KK_DEV_LEDGE_FLIGHTS=1: the run-up may be lower than the step (the steps of a flight of stairs), and Jack starts at
// the foot of the flight: 3 units before the step, on the floor there.
// Water faces do not count as floors: the flood goes on over the bottom under them, and from water deeper than Jack
// swims in (1.4 units) a bank up to 0.95 over the water counts as a step (a swim out). A step out of shallower water
// (over 0.3 units) is walked first and has its water level in ledges.csv too.
// KK_DEV_LEDGE_LIST=<ledges.csv>: tests that list instead of scanning, so every frame rate walks the same ledges.
//
// Then for each ledge and each stick (KK_DEV_LEDGE_STICKS, default "1,0.5"): Jack goes 1.2 units before it with
// the game's teleport, facing it, stands 0.5 s, then the left stick steers him at the ledge (each frame, against his
// own facing, since the stick follows the camera) until he has passed it on top of it, or for 3 s. results.csv has
// one row each. "KK dev ledge: done" ends the job.
// KK_DEV_LEDGE_TRACE=1: trace.csv has one row a frame of each walk.
// Runs from the AI pass hook (dev_sample.cpp), like dev_teleport.cpp.
#if defined(KK_DEV_TOOLS)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef near
#undef far

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>

#include "coop_pose.h"
#include "hfr.h"

REX_EXTERN(__imp__sub_827FFFA8);  // ray_cast

namespace kk {
double DevSecondsSinceMenu();
bool DevTeleportJack(PPCContext& ctx, uint8_t* base, uint32_t actor, coop::ShotVector position, float fx, float fy);

namespace {
using hfr::Load16;
using hfr::Load32;
using hfr::LoadF;
using hfr::Store32;
using hfr::StoreF;

constexpr float kGrid = 0.4f, kMinStep = 0.12f, kMaxStep = 0.95f, kRunUp = 1.2f, kFlatFloor = 0.8f;
constexpr int kRadiusCells = 200;  // 80 units
constexpr size_t kMaxNodes = 60000;
constexpr uint32_t kCurrentWorld = 0x828E8388;

struct Hit {
  bool hit = false;
  float x = 0, y = 0, z = 0, nz = 0;
  bool water = false;   // a water surface lies over this floor
  float water_z = 0;
};

struct Ledge {
  float ax, ay, az, bx, by, bz;
  float water = -1e9f;  // the water over a, for a swim out
};

struct Node {
  int ix, iy;
  float z;
};

enum class Phase { kWait, kScan, kPlace, kSettle, kWalk, kDone };

std::atomic<float> g_stick_x{0}, g_stick_y{0};

bool Ok(uint32_t a) { return a >= 0x40000000 && a < 0xA0000000 && (a & 3) == 0; }

// Jack, when he is the camera's actor and runs the player tracks.
uint32_t Jack(const uint8_t* base) {
  const uint32_t actor = Load32(base, 0x82CCBF7C);
  if (!Ok(actor) || (Load32(base, actor) & 0x00400001u)) return 0;
  const uint32_t ext = Load32(base, actor + 24);
  if (!Ok(ext)) return 0;
  const uint32_t ai = Load32(base, ext + 4);
  if (!Ok(ai)) return 0;
  const uint32_t first = Load16(base, ai + 80);
  if (first > Load16(base, ai + 82) || first > 4096) return 0;
  const uint32_t block = Load32(base, ai + (first + 6) * 4);
  if (!Ok(block) || Load32(base, block + 20) != 0x826718B8) return 0;  // H_TRACK_tagoff
  return Ok(Load32(base, actor + 12)) ? actor : 0;
}

// The game's ray_cast through every collision object of the world but Jack (mode 16: no object filter).
Hit Ray(PPCContext& ctx, uint8_t* base, uint32_t actor, float x, float y, float z, float dx, float dy, float dz,
        float len) {
  Hit out;
  const uint32_t world = Load32(base, kCurrentWorld);
  if (!Ok(world)) return out;
  const PPCContext saved = ctx;
  constexpr uint32_t kFrame = 1024;
  const uint32_t frame = (ctx.r1.u32 - kFrame) & ~15u;
  static uint8_t keep[kFrame];
  std::memcpy(keep, base + frame, kFrame);
  std::memset(base + frame, 0, kFrame);
  const uint32_t start = frame + 0x200, dir = frame + 0x210;
  StoreF(base, start, x); StoreF(base, start + 4, y); StoreF(base, start + 8, z);
  StoreF(base, dir, dx); StoreF(base, dir + 4, dy); StoreF(base, dir + 8, dz);
  ctx.r1.u64 = frame;
  ctx.r3.u64 = world;
  ctx.r4.u64 = actor;
  ctx.r5.u64 = start;
  ctx.r6.u64 = dir;
  ctx.f1.f64 = len;
  ctx.r8.u64 = 0;
  ctx.r9.u64 = 0;
  ctx.r10.u64 = 16;
  __imp__sub_827FFFA8(ctx, base);
  if (ctx.r3.u32 != 0) {
    out.hit = true;
    out.x = LoadF(base, world + 0x1626C);
    out.y = LoadF(base, world + 0x16270);
    out.z = LoadF(base, world + 0x16274);
    out.nz = LoadF(base, world + 0x16280);
    const uint32_t face = Load32(base, world + 0x162A0);
    out.water = Ok(face) && (Load32(base, face) & 0x10) != 0;
  }
  std::memcpy(base + frame, keep, kFrame);
  ctx = saved;
  return out;
}

Hit Floor(PPCContext& ctx, uint8_t* base, uint32_t actor, float x, float y, float from) {
  Hit h = Ray(ctx, base, actor, x, y, from, 0, 0, -1, 6.0f);
  if (h.hit && h.water) {
    const float water = h.z;
    h = Ray(ctx, base, actor, x, y, water - 0.02f, 0, 0, -1, 8.0f);
    h.water = true;
    h.water_z = water;
  }
  return h;
}

std::string Folder() {
  const char* v = std::getenv("KK_DEV_LEDGE");
  return v ? v : "";
}

std::vector<float> Sticks() {
  std::vector<float> out;
  const char* v = std::getenv("KK_DEV_LEDGE_STICKS");
  std::stringstream all(v && *v ? v : "1,0.5");
  for (std::string s; std::getline(all, s, ',');) out.push_back(float(std::atof(s.c_str())));
  return out;
}

std::vector<Ledge> ReadList(const char* path) {
  std::vector<Ledge> out;
  std::ifstream in(path);
  for (std::string line; std::getline(in, line);) {
    Ledge l;
    if (std::sscanf(line.c_str(), "%f,%f,%f,%f,%f,%f,%f", &l.ax, &l.ay, &l.az, &l.bx, &l.by, &l.bz, &l.water) >= 6)
      out.push_back(l);
  }
  return out;
}

// A step from a to b worth testing: flat floors, a free run-up and room over the step.
bool Testable(PPCContext& ctx, uint8_t* base, uint32_t actor, const Ledge& l, float a_nz, float b_nz) {
  if (a_nz < kFlatFloor || b_nz < kFlatFloor) return false;
  const float dx = (l.bx - l.ax) / kGrid, dy = (l.by - l.ay) / kGrid;
  const float sx = l.ax - dx * kRunUp, sy = l.ay - dy * kRunUp;
  const Hit run = Floor(ctx, base, actor, sx, sy, l.az + 1.0f);
  static const bool flights = std::getenv("KK_DEV_LEDGE_FLIGHTS") != nullptr;
  if (flights) {
    if (!run.hit || l.az - run.z < 0.1f || l.az - run.z > 1.5f) return false;
  } else {
    if (!run.hit || run.nz < kFlatFloor || std::fabs(run.z - l.az) > 0.15f) return false;
    if (Ray(ctx, base, actor, sx, sy, l.az + 0.6f, dx, dy, 0, kRunUp).hit) return false;
  }
  if (Ray(ctx, base, actor, l.ax, l.ay, l.bz + 0.6f, dx, dy, 0, 1.2f).hit) return false;
  if (Ray(ctx, base, actor, l.bx + dx * 0.4f, l.by + dy * 0.4f, l.bz + 0.1f, 0, 0, 1, 2.0f).hit) return false;
  return true;
}

}  // namespace

// The dev pad's left stick; false when no ledge is being walked.
bool DevLedgeStick(float& x, float& y) {
  x = g_stick_x.load(std::memory_order_relaxed);
  y = g_stick_y.load(std::memory_order_relaxed);
  return x != 0 || y != 0;
}

std::atomic<uint32_t> g_frame{0};

void DevContactRow(uint8_t* base, uint32_t a, uint32_t n, uint32_t push, uint32_t flags, uint32_t result) {
  constexpr uint32_t kSweep = 0x828E89D0;
  static float box[4] = {};
  static FILE* out = [] {
    const char* v = std::getenv("KK_DEV_CONTACTS");
    if (!v || std::sscanf(v, "%f,%f,%f,%f", &box[0], &box[1], &box[2], &box[3]) != 4) return static_cast<FILE*>(nullptr);
    return std::fopen((Folder() + "/contacts.csv").c_str(), "w");
  }();
  if (!out) return;
  const float x = LoadF(base, kSweep + 0x2C), y = LoadF(base, kSweep + 0x30), z = LoadF(base, kSweep + 0x34);
  const uint32_t actor = Load32(base, 0x82CCBF7C);
  if (!Ok(actor) || !Ok(Load32(base, actor + 12))) return;
  const uint32_t m = Load32(base, actor + 12);
  const float jx = LoadF(base, m + 48), jy = LoadF(base, m + 52);
  if (jx < box[0] || jx > box[1] || jy < box[2] || jy > box[3]) return;
  std::fprintf(out, "%u,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,", g_frame.load(), x, y, z, LoadF(base, kSweep + 0x38),
               LoadF(base, kSweep + 0x3C), LoadF(base, kSweep + 0x40));
  std::fprintf(out, "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%08X,%u,%.4f\n", LoadF(base, a), LoadF(base, a + 4),
               LoadF(base, a + 8), LoadF(base, n), LoadF(base, n + 4), LoadF(base, n + 8), LoadF(base, push),
               LoadF(base, push + 4), LoadF(base, push + 8), Load32(base, flags), result, LoadF(base, kSweep + 0x54));
  std::fprintf(out, "obj,%08X,jack,%08X\n", Load32(base, kSweep + 8), actor);
  std::fflush(out);
}

void SpotsTick(uint8_t* base) {
  static FILE* out = [] {
    const char* v = std::getenv("KK_DEV_SPOTS");
    return v ? std::fopen((std::string(v) + "/spots.csv").c_str(), "a") : nullptr;
  }();
  if (!out) return;
  const uint32_t actor = Jack(base);
  if (!actor) return;
  static int marks = 0, rows = 0;
  static bool held = false;
  const bool down = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
  if (down && !held) {
    ++marks;
    REXLOG_INFO("KK dev spot: mark {}", marks);
  }
  held = down;
  const uint32_t m = Load32(base, actor + 12);
  const uint32_t game = Load32(base, 0x82CC97C0);
  std::fprintf(out, "%.3f,%.5f,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%d\n", double(GetTickCount64()) / 1000, hfr::FrameTime(base),
               Ok(game) ? Load32(base, game + 8) : 0, LoadF(base, m + 48), LoadF(base, m + 52), LoadF(base, m + 56),
               -LoadF(base, m + 16), -LoadF(base, m + 20), marks);
  if (++rows % 240 == 0 || down) std::fflush(out);
}

void DevLedgeTick(PPCContext* ctx, uint8_t* base) {
  ++g_frame;
  SpotsTick(base);
  static const std::string folder = Folder();
  if (folder.empty() || !ctx) return;
  static Phase phase = Phase::kWait;
  static std::vector<Ledge> ledges;
  static std::vector<Ledge> found[3];
  static const std::vector<float> sticks = Sticks();
  static std::deque<Node> open;
  static std::unordered_map<uint64_t, float> seen;
  static float origin_x = 0, origin_y = 0;
  static size_t ledge_i = 0, stick_i = 0, tested = 0, passed = 0;
  static double phase_at = 0;
  static float start_x = 0, start_y = 0, top_z = 0;
  static FILE* results = nullptr;
  if (phase == Phase::kDone) return;
  const double t = DevSecondsSinceMenu();
  if (t < 0) return;
  const uint32_t actor = Jack(base);
  if (!actor) return;
  const uint32_t m = Load32(base, actor + 12);
  const float px = LoadF(base, m + 48), py = LoadF(base, m + 52), pz = LoadF(base, m + 56);
  auto key = [](int ix, int iy, float z) {
    return (uint64_t(uint32_t(ix + 100000)) << 40) | (uint64_t(uint32_t(iy + 100000)) << 20) |
           uint64_t(uint32_t(int(std::floor(z / 2.5f)) + 1000));
  };

  if (phase == Phase::kWait) {
    static const double from = [] {
      const char* v = std::getenv("KK_DEV_LEDGE_START");
      return v ? std::atof(v) : 3.0;
    }();
    if (t < from) return;
    static std::vector<coop::ShotVector> path = [] {
      std::vector<coop::ShotVector> out;
      const char* v = std::getenv("KK_DEV_LEDGE_PATH");
      std::ifstream in(v ? v : "");
      for (std::string line; std::getline(in, line);) {
        float x, y, z;
        if (std::sscanf(line.c_str(), "%f,%f,%f", &x, &y, &z) == 3) out.push_back({x, y, z});
      }
      return out;
    }();
    static size_t path_i = 0;
    static double path_at = 0;
    if (t < path_at) return;
    if (path_i < path.size()) {
      static const double wait = [] {
        const char* v = std::getenv("KK_DEV_LEDGE_PATH_WAIT");
        return v ? std::atof(v) : 1.0;
      }();
      if (DevTeleportJack(*ctx, base, actor, path[path_i], 1, 0)) {
        REXLOG_INFO("KK dev ledge: path point {} of {}", path_i + 1, path.size());
        ++path_i;
        path_at = t + wait;
      }
      return;
    }
    if (const char* list = std::getenv("KK_DEV_LEDGE_LIST"); list && *list) {
      ledges = ReadList(list);
      REXLOG_INFO("KK dev ledge: {} ledges from the list", ledges.size());
      phase = Phase::kPlace;
    } else {
      origin_x = px; origin_y = py;
      const Hit f = Floor(*ctx, base, actor, px, py, pz + 1.0f);
      REXLOG_INFO("KK dev ledge: scan from ({:.2f},{:.2f},{:.2f}), floor hit {} z {:.3f} nz {:.3f}", px, py, pz, f.hit, f.z, f.nz);
      if (!f.hit) return;
      open.push_back({0, 0, f.z});
      seen[key(0, 0, f.z)] = f.z;
      phase = Phase::kScan;
    }
    phase_at = t;
  }

  if (phase == Phase::kScan) {
    using Clock = std::chrono::steady_clock;
    const auto until = Clock::now() + std::chrono::milliseconds(4);
    static const int wanted = [] {
      const char* v = std::getenv("KK_DEV_LEDGE_MAX");
      return v ? std::atoi(v) : 40;
    }();
    while (!open.empty() && seen.size() < kMaxNodes && Clock::now() < until) {
      const Node n = open.front();
      open.pop_front();
      const float ax = origin_x + n.ix * kGrid, ay = origin_y + n.iy * kGrid;
      const Hit here = Floor(*ctx, base, actor, ax, ay, n.z + 0.3f);
      constexpr int kDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      for (const auto& d : kDirs) {
        const int ix = n.ix + d[0], iy = n.iy + d[1];
        if (std::abs(ix) > kRadiusCells || std::abs(iy) > kRadiusCells) continue;
        const float bx = origin_x + ix * kGrid, by = origin_y + iy * kGrid;
        const Hit f = Floor(*ctx, base, actor, bx, by, n.z + kMaxStep + 0.3f);
        if (!f.hit) continue;
        const float step = f.z - n.z;
        const bool swim_out = here.water && here.water_z - n.z > 1.4f && f.z - here.water_z <= kMaxStep &&
                              (!f.water || f.water_z - f.z < 1.0f);
        static const float max_drop = [] {
          const char* v = std::getenv("KK_DEV_LEDGE_DROP");
          return v ? float(std::atof(v)) : 3.5f;
        }();
        if ((step > kMaxStep && !swim_out) || step < -max_drop) continue;
        const bool wade = here.water && !swim_out && here.water_z - n.z > 0.3f;
        static const auto band = [] {
          std::pair<float, float> b{kMinStep, kMaxStep};
          if (const char* v = std::getenv("KK_DEV_LEDGE_BAND")) std::sscanf(v, "%f,%f", &b.first, &b.second);
          return b;
        }();
        if (((step >= band.first && step <= band.second) || (swim_out && step > 0.3f)) && here.hit) {
          const Ledge l{ax, ay, n.z, bx, by, f.z, swim_out || wade ? here.water_z : -1e9f};
          if (swim_out || Testable(*ctx, base, actor, l, here.nz, f.nz)) {
            auto& bucket = found[swim_out || wade ? 0 : step >= 0.45f ? 0 : step >= 0.25f ? 1 : 2];
            static const float spacing = [] {
              const char* v = std::getenv("KK_DEV_LEDGE_SPACING");
              return v ? float(std::atof(v)) : 1.5f;
            }();
            bool near = false;
            for (const auto& group : found)
              for (const Ledge& o : group) near = near || std::hypot(o.ax - ax, o.ay - ay) < spacing;
            if (!near) bucket.push_back(l);
          }
        }
        if (seen.emplace(key(ix, iy, f.z), f.z).second) open.push_back({ix, iy, f.z});
      }
    }
    if (open.empty() || seen.size() >= kMaxNodes) {
      for (size_t i = 0; ledges.size() < size_t(wanted); ++i) {
        bool any = false;
        for (const auto& group : found)
          if (i < group.size() && ledges.size() < size_t(wanted)) {
            ledges.push_back(group[i]);
            any = true;
          }
        if (!any) break;
      }
      if (FILE* f = std::fopen((folder + "/ledges.csv").c_str(), "w")) {
        for (const Ledge& l : ledges)
          std::fprintf(f, "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", l.ax, l.ay, l.az, l.bx, l.by, l.bz, l.water);
        std::fclose(f);
      }
      REXLOG_INFO("KK dev ledge: scan over at {:.1f} s: {} floor cells, {} high, {} middle, {} low steps, {} kept", t,
                  seen.size(), found[0].size(), found[1].size(), found[2].size(), ledges.size());
      phase = Phase::kPlace;
    }
    return;
  }

  if (phase == Phase::kPlace) {
    if (ledge_i >= ledges.size()) {
      if (results) std::fclose(results);
      REXLOG_INFO("KK dev ledge: done, {} walks, {} passed", tested, passed);
      phase = Phase::kDone;
      return;
    }
    if (!results) {
      results = std::fopen((folder + "/results.csv").c_str(), "w");
      if (results) std::fprintf(results, "ledge,height,stick,passed,seconds,x,y,z,top_z,along\n");
    }
    const Ledge& l = ledges[ledge_i];
    const float dx = (l.bx - l.ax) / kGrid, dy = (l.by - l.ay) / kGrid;
    static const bool flights = std::getenv("KK_DEV_LEDGE_FLIGHTS") != nullptr;
    static const float run_up = [] {
      const char* v = std::getenv("KK_DEV_LEDGE_RUNUP");
      return v ? float(std::atof(v)) : kRunUp;
    }();
    const float back = flights ? 3.0f : run_up;
    start_x = l.ax - dx * back;
    start_y = l.ay - dy * back;
    g_stick_x = g_stick_y = 0;
    // A swimmer drops in near the surface.
    float start_z = l.water > -1e8f && l.water - l.az > 1.4f ? l.water - 1.0f : l.az + 0.05f;
    if (flights) {
      const Hit foot = Floor(*ctx, base, actor, start_x, start_y, l.az + 0.5f);
      if (!foot.hit) {
        if (++ledge_i >= ledges.size()) stick_i = 0;
        return;
      }
      start_z = foot.z + 0.05f;
    }
    if (DevTeleportJack(*ctx, base, actor, {start_x, start_y, start_z}, dx, dy)) {
      top_z = l.az;
      phase = Phase::kSettle;
      phase_at = t;
    }
    return;
  }

  if (phase == Phase::kSettle) {
    if (t - phase_at < 0.5) return;
    phase = Phase::kWalk;
    phase_at = t;
  }

  // Walking.
  static FILE* trace = [] {
    return std::getenv("KK_DEV_LEDGE_TRACE") ? std::fopen((Folder() + "/trace.csv").c_str(), "w") : nullptr;
  }();
  if (trace)
    std::fprintf(trace, "%zu,%.2f,%.4f,%.5f,%.5f,%.5f,%.5f\n", ledge_i, sticks[stick_i], t - phase_at, hfr::FrameTime(base), px, py, pz);
  const Ledge& l = ledges[ledge_i];
  const float dx = (l.bx - l.ax) / kGrid, dy = (l.by - l.ay) / kGrid;
  const float along = (px - start_x) * dx + (py - start_y) * dy;
  top_z = std::max(top_z, pz);
  const float height = l.bz - l.az;
  const float run_up = std::hypot(l.ax - start_x, l.ay - start_y);
  const bool over = along >= run_up + kGrid + 0.2f && pz >= l.bz - 0.12f;
  static const double time_limit = [] {
    const char* v = std::getenv("KK_DEV_LEDGE_TIME");
    return v ? std::atof(v) : 3.0;
  }();
  if (!over && t - phase_at < time_limit) {
    // Toward a point past the ledge, in Jack's own axes (his forward is the second matrix row, negated).
    const float fx = -LoadF(base, m + 16), fy = -LoadF(base, m + 20), flen = std::hypot(fx, fy);
    float wx = l.bx + dx - px, wy = l.by + dy - py;
    const float wlen = std::hypot(wx, wy);
    if (flen > 0 && wlen > 0) {
      wx /= wlen; wy /= wlen;
      const float nx = fx / flen, ny = fy / flen;
      g_stick_x = sticks[stick_i] * (wx * ny - wy * nx);
      g_stick_y = sticks[stick_i] * (wx * nx + wy * ny);
    }
    return;
  }
  g_stick_x = g_stick_y = 0;
  ++tested;
  passed += over ? 1 : 0;
  if (results) {
    std::fprintf(results, "%zu,%.3f,%.2f,%d,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f\n", ledge_i, height, sticks[stick_i], over ? 1 : 0,
                 t - phase_at, px, py, pz, top_z, along);
    std::fflush(results);
    if (trace) std::fflush(trace);
  }
  if (++stick_i >= sticks.size()) {
    stick_i = 0;
    ++ledge_i;
  }
  phase = Phase::kPlace;
}

}  // namespace kk

#endif
