// Developer-only: chapter jump investigation (never shipped). Findings:
// native-renderer/docs/chapter-jump.md.
//
// KK_DEV_JUMP=<entry>: start chapter-select entry <entry> straight from the
// title screen (no save menu, no main menu), the way the chapter select does.
// KK_DEV_JUMP_WAIT (uni_go calls to wait first, default 1), KK_DEV_JUMP_MENU=1
// (wait for the start menu instead), KK_DEV_JUMP_SLOT, KK_DEV_JUMP_SNAPSHOT=0,
// KK_DEV_JUMP_REINIT=0, KK_DEV_JUMP_END, KK_DEV_JUMP_DUMPG and
// KK_DEV_JUMP_WALLSHOTS are test variants (described where they are used).
//
// KK_DEV_JUMP_TRACE=1 logs the universe's level requests: uni_go
// (sub_8253DB80, the script function that performs a level change) runs every
// frame and acts when the universe variables G (*(*(0x82CCBF64)+0x40), also
// *(0x82CC97C0) once a level is up) ask for a load: G+4 = 1 (request), G+8 =
// the level (map) key, G+0xC = the start point. It also logs when the init
// functions that fill the chapter table run.
#if defined(KK_DEV_TOOLS)

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/logging.h>

#include "art.h"
#include "guest.h"

namespace kk {
void DevStartScriptClock();  // dev_tools.cpp
}

namespace kk::jump {
namespace {

constexpr uint32_t kGamePtr = 0x82CC97C0, kProfilePtr = 0x82CC8D24, kAiPtr = 0x82CCBF64;

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  const uint8_t* p = base + addr;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

bool Tracing() {
  static const bool on = std::getenv("KK_DEV_JUMP_TRACE") != nullptr;
  return on;
}

std::string Vars(const uint8_t* base) {
  const uint32_t gp = Load32(base, kGamePtr), p = Load32(base, kProfilePtr), ai = Load32(base, kAiPtr);
  const uint32_t v = ai ? Load32(base, ai + 0x40) : 0;
  const uint32_t g = gp ? gp : v;  // on the title screen the globals are 0; uni_go's own block then
  if (!g) return fmt::format("G=0 P={:08X} v=0", p);
  return fmt::format(
      "G={:08X} P={:08X} v={:08X} | go {} map {} start {} | 4e08 {} | chapter mode {} chapter {} | reached {} "
      "count {} | 5678 {} 567c {} 1c {} flags {:08X}",
      gp, p, v, int32_t(Load32(base, g + 4)), int32_t(Load32(base, g + 8)), int32_t(Load32(base, g + 0xC)),
      int32_t(Load32(base, g + 0x4e08)), int32_t(Load32(base, g + 0x5668)), int32_t(Load32(base, g + 0x5670)),
      int32_t(Load32(base, g + 0x4ff4)), int32_t(Load32(base, g + 0x5000)), int32_t(Load32(base, g + 0x5678)),
      int32_t(Load32(base, g + 0x567c)), int32_t(Load32(base, g + 0x1c)), Load32(base, g + 0xbf0));
}

void DumpTable(const uint8_t* base) {
  const uint32_t g = Load32(base, kGamePtr);
  if (!g) return;
  const int count = int(Load32(base, g + 0x5000));
  for (int i = 0; i < count && i < 51; ++i) {
    std::string row;
    for (uint32_t col = 0x1402; col <= 0x1567; col += 0x33) row += fmt::format(" {}", int32_t(Load32(base, g + (col + i) * 4)));
    REXLOG_INFO("KK dev jump: chapter {:2}:{}", i, row);
  }
}

void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  uint8_t* p = base + addr;
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

int EnvInt(const char* name, int fallback) {
  const char* v = std::getenv(name);
  return v && *v ? std::atoi(v) : fallback;
}

// KK_DEV_JUMP_WALLSHOTS="10,20,40": save the game's frame that many seconds
// after the program started, on a wall clock (the dev shots need pad reads,
// and a long load reads no pad). Also logs, every second, whether a movie is
// playing (the player at *(0x82CCAE80), +268/+272) and the loading screen state.
const bool g_wall_shots = [] {
  const char* v = std::getenv("KK_DEV_JUMP_WALLSHOTS");
  if (!v || !*v) return false;
  std::vector<double> times;
  std::stringstream all(v);
  for (std::string t; std::getline(all, t, ',');) times.push_back(std::stod(t));
  std::thread([times] {
    const auto start = std::chrono::steady_clock::now();
    size_t next = 0;
    for (int tick = 0;; ++tick) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      while (next < times.size() && t >= times[next]) {
        const char* dir = std::getenv("KK_DEV_SHOTS_DIR");
        kk::art::CaptureFrame(std::filesystem::path(dir && *dir ? dir : ".") /
                              ("wall_" + std::to_string(int(times[next] * 10)) + ".bmp"));
        ++next;
      }
      if (tick % 10 == 0 && kk::g_guest_base) {
        const uint8_t* base = kk::g_guest_base;
        const uint32_t player = Load32(base, 0x82CCAE80);
        REXLOG_INFO("KK dev jump: {:.1f} s: movie {} {} | loading screen thread {:08X} state {}", t,
                    player ? Load32(base, player + 268) : 0, player ? Load32(base, player + 272) : 0,
                    Load32(base, 0x82D64080), int32_t(Load32(base, 0x82D64080 + 12)));
      }
      if (next >= times.size() && t > 300) return;
    }
  }).detach();
  return true;
}();

bool g_menu_ready = false;  // IntMIG_init has run: the start menu's map is up
int g_frames_ready = 0;

}  // namespace
}  // namespace kk::jump

REX_EXTERN(__imp__sub_82413190);
REX_EXTERN(__imp__sub_8271D698);

namespace kk::jump {
namespace {

// What pressing A on chapter-select entry `idx` does (IntMIG_Page_Map,
// sub_824BD150, the branch for a chapter other than the one in progress),
// followed by what the load page (IntMIG_Page_load, sub_824B4DF0) does with
// it: ask the universe for the chapter's level.
void StartChapter(PPCContext& ctx, uint8_t* base, uint32_t g, int idx) {
  const PPCContext saved = ctx;
  if (EnvInt("KK_DEV_JUMP_SNAPSHOT", 1)) {
    __imp__sub_82413190(ctx, base);                                  // snapshot the universe variables
    std::memmove(base + 0x82C58490, base + 0x82C63EA0, 0x7F40);      // keep it as the chapter's restart copy
    std::memmove(base + 0x82C603D0, base + 0x82C6399C, 0x500);
    if (EnvInt("KK_DEV_JUMP_REINIT", 1)) {
      ctx.r3.u64 = Load32(base, 0x828E838C);                         // the universe's script instance
      __imp__sub_8271D698(ctx, base);
    }
    ctx = saved;
    __imp__sub_82413190(ctx, base);
    ctx = saved;
  }
  // KK_DEV_JUMP_SLOT=n: also claim save slot n (G+0x4E10, which the save
  // menu page sets when a save is chosen; -1 = none). Test only: the slot's
  // save is not loaded, so a later save would overwrite it with this state.
  if (const char* slot = std::getenv("KK_DEV_JUMP_SLOT"); slot && *slot) Store32(base, g + 0x4E10, uint32_t(std::atoi(slot)));
  Store32(base, 0x828D177C, Load32(base, g + (0x149B + idx) * 4));
  Store32(base, g + 0x5668, 1);                                      // started from the chapter select
  Store32(base, g + 0x5670, uint32_t(idx));                          // which entry
  const uint32_t map = Load32(base, g + (0x1402 + idx) * 4), start = Load32(base, g + (0x1435 + idx) * 4);
  // The load page: request the level.
  Store32(base, g + 8, map);
  Store32(base, g + 0xC, start);
  Store32(base, 0x828E3B48, 1);
  Store32(base, g + 0x5F1C, 0);
  Store32(base, g + 4, 1);
  REXLOG_INFO("KK dev jump: starting chapter-select entry {} (map {}, start {}): {}", idx, int32_t(map),
              int32_t(start), Vars(base));
}

}  // namespace
}  // namespace kk::jump

// uni_go: performs a requested level change.
REX_EXTERN(__imp__sub_8253DB80);
REX_HOOK_RAW(sub_8253DB80) {
  using namespace kk::jump;
  // The universe's variables, as uni_go reads them (the script globals
  // *(0x82CC97C0) and *(0x82CC8D24) point at the same block once the start
  // menu has run, but are still 0 on the title screen).
  const uint32_t ai = Load32(base, kAiPtr);
  const uint32_t g = ai ? Load32(base, ai + 0x40) : 0;
  // KK_DEV_JUMP=<entry>: once the start menu's map is up (and KK_DEV_JUMP_WAIT
  // more frames of uni_go have passed), start that chapter-select entry.
  static const int jump = EnvInt("KK_DEV_JUMP", -1);
  static bool jumped = false;
  static int calls = 0;
  if (Tracing() && (calls++ % 300) == 0) REXLOG_INFO("KK dev jump: uni_go call {}: {}", calls, Vars(base));
  if (jump >= 0 && !jumped && g && (g_menu_ready || !EnvInt("KK_DEV_JUMP_MENU", 0)) && Load32(base, g + 4) == 0 &&
      ++g_frames_ready > EnvInt("KK_DEV_JUMP_WAIT", 1)) {
    jumped = true;
    const int count = int(Load32(base, g + 0x5000));
    if (jump < count) {
      if (EnvInt("KK_DEV_JUMP_SETG", 0)) {
        for (uint32_t ptr : {kGamePtr, kProfilePtr})
          if (!Load32(base, ptr)) Store32(base, ptr, g);
      }
      StartChapter(ctx, base, g, jump);
    }
    else REXLOG_WARN("KK dev jump: entry {} out of range ({} chapters)", jump, count);
  }
  const bool request = g && Load32(base, g + 4) != 0;
  if (Tracing() && request) {
    REXLOG_INFO("KK dev jump: uni_go request lr={:08X}: {}", uint32_t(ctx.lr), Vars(base));
    DumpTable(base);
  }
  __imp__sub_8253DB80(ctx, base);
  if (Tracing() && request) REXLOG_INFO("KK dev jump: uni_go done: {}", Vars(base));
  // KK_DEV_JUMP_DUMPG=<seconds>: that long after a chapter-select level has
  // loaded (by a jump or through the menus), write the universe variables
  // (0x8000 bytes from G) to universe.bin in KK_DEV_SHOTS_DIR, to compare.
  static std::chrono::steady_clock::time_point loaded{};
  if (request && Load32(base, g + 0x5668) == 1) loaded = std::chrono::steady_clock::now();
  if (static bool dumped = false; !dumped && loaded != std::chrono::steady_clock::time_point{} &&
                                  std::getenv("KK_DEV_JUMP_DUMPG") &&
                                  std::chrono::steady_clock::now() - loaded >
                                      std::chrono::duration<double>(std::atof(std::getenv("KK_DEV_JUMP_DUMPG")))) {
    dumped = true;
    const char* dir = std::getenv("KK_DEV_SHOTS_DIR");
    const std::string path = (std::filesystem::path(dir && *dir ? dir : ".") / "universe.bin").string();
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(base + g, 1, 0x8000, f);
      std::fclose(f);
      REXLOG_INFO("KK dev jump: wrote the universe variables ({:08X}) to {}", g, path);
    }
  }
  // KK_DEV_JUMP_END=<seconds>: that long after the jump's level loaded, ask
  // for the next chapter's level, as reaching the chapter's end does (uni_go
  // then handles it as the end of a chapter-select run).
  static std::chrono::steady_clock::time_point jump_loaded{};
  if (static bool ended = false; !ended && jumped && jump_loaded != std::chrono::steady_clock::time_point{} &&
                                 std::getenv("KK_DEV_JUMP_END") && Load32(base, g + 4) == 0 &&
                                 std::chrono::steady_clock::now() - jump_loaded >
                                     std::chrono::duration<double>(std::atof(std::getenv("KK_DEV_JUMP_END")))) {
    ended = true;
    const int next = jump + 1;
    Store32(base, g + 8, Load32(base, g + (0x1402 + next) * 4));
    Store32(base, g + 0xC, Load32(base, g + (0x1435 + next) * 4));
    Store32(base, g + 4, 1);
    REXLOG_INFO("KK dev jump: asking for the next chapter's level (entry {}): {}", next, Vars(base));
  }
  if (request && jumped) {
    static bool clock = false;
    if (!clock) {
      clock = true;
      REXLOG_INFO("KK dev jump: the chapter's level is loaded");
      jump_loaded = std::chrono::steady_clock::now();
      kk::DevStartScriptClock();
    }
  }
}

#define KK_JUMP_LOG_HOOK(addr, name)                                                          \
  REX_EXTERN(__imp__sub_##addr);                                                              \
  REX_HOOK_RAW(sub_##addr) {                                                                  \
    using namespace kk::jump;                                                                 \
    if (Tracing()) REXLOG_INFO("KK dev jump: {} in: {}", name, Vars(base));                   \
    __imp__sub_##addr(ctx, base);                                                             \
    if (Tracing()) REXLOG_INFO("KK dev jump: {} out: {}", name, Vars(base));                  \
  }

REX_EXTERN(__imp__sub_824AE560);
REX_HOOK_RAW(sub_824AE560) {
  using namespace kk::jump;
  if (Tracing()) REXLOG_INFO("KK dev jump: IntMIG_init in: {}", Vars(base));
  __imp__sub_824AE560(ctx, base);
  g_menu_ready = true;
  if (Tracing()) REXLOG_INFO("KK dev jump: IntMIG_init out: {}", Vars(base));
}

KK_JUMP_LOG_HOOK(824FED10, "exec_universe_initvars")
KK_JUMP_LOG_HOOK(8253E398, "uni_init")
KK_JUMP_LOG_HOOK(82521F28, "GST_MM_Init")

#endif
