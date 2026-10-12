// Developer-only: samples game state over real time, to compare runs at
// different frame rates (docs/high-frame-rate.md, tools/hfr_compare.py).
//
// KK_DEV_SAMPLE="from,to[,interval]" (seconds after the save menu opens; the
// interval defaults to 0.05 s): from the first frame after each interval,
// writes the world's active objects to sample.csv in the working folder (or
// KK_DEV_SAMPLE_DIR), one row per object:
//   t (s since the save menu), frame (game frames since the sampling began),
//   gt (sum of the frame times since this world was first seen, s: the
//   level's own clock, to line runs up), dt, i (index in the active list),
//   obj (guest address), flags (+0), x y z (global matrix translation),
//   ax ay az (its first axis), ev_t ev_i (animation: frame in seconds, and
//   current action * 256 + action item),
//   ai_t ai_f (first AI track: wait timer, its compiled function),
//   key (the object's file key, stable between runs; 0 if not found)
// and every frame one row with i = -1 and obj = the world (the frame's dt), so
// frame pacing and the sum of frame times can be checked too.
//
// KK_DEV_SAMPLE_GLOBALS="82D1CDC8,828E8378/i": also writes those guest
// addresses (floats, or 32-bit integers with /i) every frame to globals.csv.
//
// KK_DEV_SAMPLE_DUMP="t1,t2" (same clock): at those times writes the raw bytes
// of each active object and of the blocks it points to (dump_<t>.txt), to find
// where things live.
//
// The engine's per-world call (sub_82881B70) runs the AI of every active
// object with sub_8271D498(world); the hook here samples before it. World
// layout: +292 end and +304 start of the active objects table (8 bytes per
// entry, the object first). Object: +0 identity flags, +12 matrices (global
// first, 80 bytes each: axes, then translation at +48), +20 base, +24
// extended (+4 AI instance, +32 event tracks).

#if defined(KK_DEV_TOOLS)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>

#include "hfr.h"

REX_EXTERN(__imp__sub_823F8AF0);  // the animation frame of an object's layer

namespace kk {
double DevSecondsSinceMenu();

namespace {

using hfr::Load16;
using hfr::Load32;
using hfr::LoadF;

std::vector<double> ParseList(const char* v) {
  std::vector<double> out;
  if (!v || !*v) return out;
  std::stringstream all(v);
  for (std::string s; std::getline(all, s, ',');) out.push_back(std::stod(s));
  return out;
}

std::filesystem::path OutDir() {
  const char* d = std::getenv("KK_DEV_SAMPLE_DIR");
  return d && *d ? d : ".";
}

// A guest address that points into ordinary game memory.
bool Valid(uint32_t a) { return a >= 0x40000000 && a < 0xA0000000 && (a & 3) == 0; }
bool ValidAny(uint32_t a) { return a >= 0x40000000 && a < 0xA0000000; }

// Loaded files by key (BIG keys, stable between runs): sub_827061B0(key)
// looks the key up in the sorted {key, index} table at 0x82C4ED94 ({entries,
// count}), then the index in 256 buckets at 0x82C4EDC0 (16 bytes each:
// {entries, count}; {index, address} pairs) to get the loaded address.
std::unordered_map<uint32_t, uint32_t> KeysByAddress(const uint8_t* base) {
  std::unordered_map<uint32_t, uint32_t> index_to_address, out;
  for (uint32_t b = 0; b < 256; ++b) {
    const uint32_t list = Load32(base, 0x82C4EDC0 + b * 16), n = Load32(base, 0x82C4EDC0 + b * 16 + 4);
    if (!Valid(list) || n > 100000) continue;
    for (uint32_t k = 0; k < n; ++k) index_to_address[Load32(base, list + k * 8)] = Load32(base, list + k * 8 + 4);
  }
  const uint32_t keys = Load32(base, 0x82C4ED94), n = Load32(base, 0x82C4ED98);
  if (!Valid(keys) || n > 1000000) return out;
  static bool logged = false;
  for (uint32_t k = 0; k < n; ++k) {
    const auto it = index_to_address.find(Load32(base, keys + k * 8 + 4));
    if (it != index_to_address.end()) out[it->second] = Load32(base, keys + k * 8);
  }
  if (!logged) {
    logged = true;
    REXLOG_INFO("KK dev sample: {} keys, {} loaded indices, {} addresses", n, index_to_address.size(), out.size());
  }
  return out;
}

struct Sampler {
  std::vector<double> range = ParseList(std::getenv("KK_DEV_SAMPLE"));
  std::vector<double> dumps = ParseList(std::getenv("KK_DEV_SAMPLE_DUMP"));
  FILE* f = nullptr;
  double next = 0;
  uint64_t frame = 0;
  double gt = 0;
  uint32_t last_world = 0;
  // KK_DEV_SAMPLE_GLOBALS="82D1CDC8,82D623E0/i,..." (hex guest addresses; /i
  // reads a 32-bit integer, else a float): every frame to globals.csv.
  std::vector<std::pair<uint32_t, bool>> globals = [] {
    std::vector<std::pair<uint32_t, bool>> out;
    if (const char* v = std::getenv("KK_DEV_SAMPLE_GLOBALS"); v && *v) {
      std::stringstream all(v);
      for (std::string s; std::getline(all, s, ',');)
        out.push_back({uint32_t(std::stoul(s, nullptr, 16)), s.find("/i") != std::string::npos});
    }
    return out;
  }();
  FILE* g = nullptr;

  void Globals(const uint8_t* base, double t, float dt) {
    if (globals.empty()) return;
    if (!g) {
      g = std::fopen((OutDir() / "globals.csv").string().c_str(), "w");
      if (!g) {
        globals.clear();
        return;
      }
      std::fprintf(g, "t,gt,dt");
      for (const auto& [a, is_int] : globals) std::fprintf(g, ",%08X", a);
      std::fprintf(g, "\n");
    }
    std::fprintf(g, "%.4f,%.4f,%.5f", t, gt, dt);
    for (const auto& [a, is_int] : globals) {
      if (is_int)
        std::fprintf(g, ",%d", int32_t(Load32(base, a)));
      else
        std::fprintf(g, ",%.6g", LoadF(base, a));
    }
    std::fprintf(g, "\n");
  }
  std::unordered_map<uint32_t, uint32_t> keys;
  double keys_at = -100;

  bool on() const { return range.size() >= 2 || !dumps.empty(); }

  void Objects(PPCContext* ctx, uint8_t* base, uint32_t world, double t, float dt, bool rows) {
    if (t - keys_at > 2.0) {
      keys = KeysByAddress(base);
      keys_at = t;
    }
    const uint32_t end = Load32(base, world + 292), start = Load32(base, world + 304);
    if (!Valid(start) || !Valid(end) || end < start || end - start > 8 * 20000) return;
    int i = 0;
    for (uint32_t e = start; e + 8 <= end; e += 8, ++i) {
      const uint32_t obj = Load32(base, e);
      if (!Valid(obj) || (obj & 3) == 1) continue;
      const uint32_t m = Load32(base, obj + 12);
      float x = 0, y = 0, z = 0, ax = 0, ay = 0, az = 0;
      if (Valid(m)) {
        x = LoadF(base, m + 48), y = LoadF(base, m + 52), z = LoadF(base, m + 56);
        ax = LoadF(base, m + 0), ay = LoadF(base, m + 4), az = LoadF(base, m + 8);
      }
      float ev_t = 0, ai_t = 0;
      int ev_i = -1;
      uint32_t ai_f = 0;
      // Animation: [[obj+20]+0] is the object's animation block: +156 the
      // current action, +160 its item, +132 the first layer's animation. Its
      // frame (1/60 s units) comes from the game's own sub_823F8AF0(obj, 0).
      if (const uint32_t b = Load32(base, obj + 20); Valid(b)) {
        if (const uint32_t anim = Load32(base, b); Valid(anim)) {
          ev_i = int(Load16(base, anim + 156)) * 256 + base[anim + 160];
          const uint32_t layer = Load32(base, anim + 132);
          if (ctx && Valid(layer) && Valid(Load32(base, layer + 4))) {
            PPCContext saved = *ctx;
            ctx->r3.u64 = obj;
            ctx->r4.u64 = 0;
            __imp__sub_823F8AF0(*ctx, base);
            ev_t = float(int32_t(ctx->r3.u32)) / 60.0f;
            *ctx = saved;
          }
        }
      }
      if (const uint32_t ext = Load32(base, obj + 24); Valid(ext)) {
        if (const uint32_t ai = Load32(base, ext + 4); Valid(ai)) {
          if (Load16(base, ai + 82) >= Load16(base, ai + 80)) {
            const uint32_t first = Load16(base, ai + 80);
            ai_t = LoadF(base, ai + (first + 11) * 4);
            // The track's function block: +20 its compiled function (names: the
            // script tables in the image, {hash, function, name}).
            const uint32_t block = Load32(base, ai + (first + 6) * 4);
            ai_f = Valid(block) ? Load32(base, block + 20) : 0;
          }
        }
      }
      if (rows && f)
        std::fprintf(f, "%.4f,%llu,%.4f,%.5f,%d,%08X,%08X,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%.4f,%08X,%08X\n", t,
                     static_cast<unsigned long long>(frame), gt, dt, i, obj, Load32(base, obj), x, y, z, ax, ay, az,
                     ev_t, ev_i, ai_t, ai_f, keys.count(obj) ? keys[obj] : 0u);
    }
  }

  void Dump(uint8_t* base, uint32_t world, double at) {
    char name[64];
    std::snprintf(name, sizeof(name), "dump_%d.txt", int(at * 10));
    FILE* d = std::fopen((OutDir() / name).string().c_str(), "w");
    if (!d) return;
    auto hex = [&](const char* label, uint32_t a, uint32_t n) {
      if (!Valid(a)) return;
      std::fprintf(d, "  %s %08X:", label, a);
      for (uint32_t k = 0; k < n; k += 4) std::fprintf(d, "%s%08X", k % 32 ? " " : "\n    ", Load32(base, a + k));
      std::fprintf(d, "\n");
    };
    std::fprintf(d, "world %08X\n", world);
    hex("world", world, 512);
    const uint32_t end = Load32(base, world + 292), start = Load32(base, world + 304);
    int i = 0;
    for (uint32_t e = start; Valid(start) && e + 8 <= end && i < 4000; e += 8, ++i) {
      const uint32_t obj = Load32(base, e);
      if (!Valid(obj)) continue;
      std::fprintf(d, "object %d %08X (entry flags %08X)\n", i, obj, Load32(base, e + 4));
      hex("obj", obj, 96);
      hex("obj+12", Load32(base, obj + 12), 256);
      hex("obj+20", Load32(base, obj + 20), 96);
      const uint32_t ext = Load32(base, obj + 24);
      hex("obj+24", ext, 96);
      if (Valid(ext)) {
        hex("ext+4 (ai)", Load32(base, ext + 4), 128);
        const uint32_t ev = Load32(base, ext + 32);
        hex("ext+32 (events)", ev, 16);
        if (Valid(ev)) hex("events+4", Load32(base, ev + 4), 64);
      }
    }
    std::fclose(d);
    REXLOG_INFO("KK dev sample: wrote {}", name);
  }

  void Tick(PPCContext* ctx, uint8_t* base, uint32_t world) {
    const double t = DevSecondsSinceMenu();
    if (t < 0) return;
    const float dt = hfr::FrameTime(base);
    for (auto it = dumps.begin(); it != dumps.end();) {
      if (t >= *it) {
        Dump(base, world, *it);
        it = dumps.erase(it);
      } else {
        ++it;
      }
    }
    if (range.size() < 2 || t < range[0]) return;
    if (t > range[1]) {
      if (f) {
        std::fclose(f);
        f = nullptr;
        REXLOG_INFO("KK dev sample: done");
      }
      if (g) {
        std::fclose(g);
        g = nullptr;
      }
      range.clear();
      return;
    }
    if (!f) {
      f = std::fopen((OutDir() / "sample.csv").string().c_str(), "w");
      if (!f) {
        range.clear();
        return;
      }
      std::fprintf(f, "t,frame,gt,dt,i,obj,flags,x,y,z,ax,ay,az,ev_t,ev_i,ai_t,ai_f,key\n");
      next = t;
      REXLOG_INFO("KK dev sample: sampling from {:.2f} s", t);
    }
    // (Levels run one world; if a second one ran, its rows would carry its own
    // address and clock.)
    if (world != last_world) {
      last_world = world;
      gt = 0;
      REXLOG_INFO("KK dev sample: world {:08X} at {:.2f} s", world, t);
    }
    ++frame;
    gt += dt;
    std::fprintf(f, "%.4f,%llu,%.4f,%.5f,-1,%08X,0,0,0,0,0,0,0,0,-1,0,0,0\n", t,
                 static_cast<unsigned long long>(frame), gt, dt, world);
    Globals(base, t, dt);
    const double interval = range.size() >= 3 ? range[2] : 0.05;
    if (t >= next) {
      Objects(ctx, base, world, t, dt, true);
      next += interval;
      if (next < t) next = t + interval;
    }
    // The game may be stopped at any time: keep the files current.
    if (t - flushed >= 1.0) {
      flushed = t;
      std::fflush(f);
      if (g) std::fflush(g);
    }
  }
  double flushed = 0;
};

Sampler& Get() {
  static Sampler s;
  return s;
}

}  // namespace

// For other dev aids that sample inside the AI pass.
void DevSampleTick(PPCContext* ctx, uint8_t* base, uint32_t world) {
  static const bool on = Get().on();
  if (on) Get().Tick(ctx, base, world);
}

}  // namespace kk

REX_EXTERN(__imp__sub_8271D498);
REX_HOOK_RAW(sub_8271D498) {
  kk::DevSampleTick(&ctx, base, ctx.r3.u32);
  __imp__sub_8271D498(ctx, base);
}

#endif

