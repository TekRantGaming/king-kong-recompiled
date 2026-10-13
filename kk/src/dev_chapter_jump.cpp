// Developer-only: traces from the chapter jump investigation (never shipped). Findings:
// native-renderer/docs/chapter-jump.md. The jump itself is now the start request (play_session.cpp,
// KK_DEV_START for tests).
//
// KK_DEV_JUMP_TRACE=1 logs the universe's level requests: uni_go
// (sub_8253DB80, the script function that performs a level change) runs every
// frame and acts when the universe variables G (*(*(0x82CCBF64)+0x40), also
// *(0x82CC97C0) once a level is up) ask for a load: G+4 = 1 (request), G+8 =
// the level (map) key, G+0xC = the start point. It also logs when the init
// functions that fill the chapter table run. KK_DEV_START_END, KK_DEV_JUMP_DUMPG,
// KK_DEV_JUMP_WALLSHOTS and KK_DEV_SAVE_TRACE are described where they are used.
#if defined(KK_DEV_TOOLS)

#include <atomic>
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

bool SaveTracingEarly() {
  static const bool on = std::getenv("KK_DEV_SAVE_TRACE") != nullptr;
  return on;
}

bool g_menu_ready = false;
std::atomic<bool> g_read_all_done{false}, g_load_slot_done{false};
std::atomic<int> g_load_slot_result{0};  // IntMIG_init has run: the start menu's map is up

}  // namespace
}  // namespace kk::jump

namespace kk {
// Called by play_session.cpp's uni_go hook after each call: `request` = a level change was asked for
// this frame, `started` = the start request's level has loaded.
void DevUniGoTick(uint8_t* base, uint32_t g, bool request, bool started) {
  using namespace kk::jump;
  // KK_DEV_SAVE_TRACE: log whenever the save-related universe variables change.
  if (SaveTracingEarly()) {
    static std::string last;
    std::string now = g ? fmt::format("slot {} player {} {} dev {} 5f24 {} flags {:08X} reached {} continue {} {} 4e14 {}",
                                      int32_t(Load32(base, g + 0x4E10)), int32_t(Load32(base, g + 0x5F00)),
                                      int32_t(Load32(base, g + 0x5F04)), int32_t(Load32(base, g + 0x5F08)),
                                      int32_t(Load32(base, g + 0x5F24)), Load32(base, g + 0xBF0),
                                      int32_t(Load32(base, g + 0x4FF4)), int32_t(Load32(base, g + 0x4FF8)),
                                      int32_t(Load32(base, g + 0x4FFC)), int32_t(Load32(base, g + 0x4E14))) +
                              fmt::format(" header0 {} {} restart copy {} {}", int32_t(Load32(base, 0x82C6399C + 0x28)),
                                          int32_t(Load32(base, 0x82C6399C + 0x2C)),
                                          int32_t(Load32(base, 0x82C73D20 + 0x28)),
                                          int32_t(Load32(base, 0x82C73D20 + 0x2C)))
                        : std::string();
    if (now != last) {
      REXLOG_INFO("KK dev save: universe now {} (memcard slot {} status {:08X})", now,
                  int32_t(Load32(base, 0x82C63E9C)), Load32(base, 0x82C8E7D0));
      last = now;
    }
  }
  static int calls = 0;
  if (Tracing() && (calls++ % 300) == 0) REXLOG_INFO("KK dev jump: uni_go call {}: {}", calls, Vars(base));
  if (Tracing() && request) {
    REXLOG_INFO("KK dev jump: uni_go handled a request: {}", Vars(base));
    DumpTable(base);
  }
  if (!g) return;
  static std::chrono::steady_clock::time_point loaded{};
  if (started && loaded == std::chrono::steady_clock::time_point{}) loaded = std::chrono::steady_clock::now();
  if (loaded == std::chrono::steady_clock::time_point{}) return;
  const auto since = std::chrono::steady_clock::now() - loaded;
  // KK_DEV_JUMP_DUMPG=<seconds>: that long after the start request's level loaded, write the universe
  // variables (0x8000 bytes from G) to universe.bin in KK_DEV_SHOTS_DIR, to compare.
  if (static bool dumped = false; !dumped && std::getenv("KK_DEV_JUMP_DUMPG") &&
                                  since > std::chrono::duration<double>(std::atof(std::getenv("KK_DEV_JUMP_DUMPG")))) {
    dumped = true;
    const char* dir = std::getenv("KK_DEV_SHOTS_DIR");
    const std::string path = (std::filesystem::path(dir && *dir ? dir : ".") / "universe.bin").string();
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(base + g, 1, 0x8000, f);
      std::fclose(f);
      REXLOG_INFO("KK dev jump: wrote the universe variables ({:08X}) to {}", g, path);
    }
  }
  // KK_DEV_TEXT_DUMP=<seconds>: that long after the start request's level loaded, write every string of
  // the game's loaded text files (the text manager at 0x828E89B8: +8 count, +12 files; a file: +4 count,
  // +8 entries of 32 bytes (+0 id, +12 offset), +12 strings, +20 key) to texts.txt in KK_DEV_SHOTS_DIR.
  if (static bool texts = false; !texts && std::getenv("KK_DEV_TEXT_DUMP") &&
                                 since > std::chrono::duration<double>(std::atof(std::getenv("KK_DEV_TEXT_DUMP")))) {
    texts = true;
    const char* dir = std::getenv("KK_DEV_SHOTS_DIR");
    const std::string path = (std::filesystem::path(dir && *dir ? dir : ".") / "texts.txt").string();
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      const uint32_t mgr = 0x828E89B8, files = Load32(base, mgr + 12);
      const int count = int(Load32(base, mgr + 8));
      for (int i = 0; i < count && i < 4096 && files; ++i) {
        const uint32_t file = Load32(base, files + 4 * uint32_t(i));
        if (!file) continue;
        const uint32_t n = Load32(base, file + 4), entries = Load32(base, file + 8), strings = Load32(base, file + 12);
        std::fprintf(f, "file %d key %08X entries %u\n", i, Load32(base, file + 20), n);
        for (uint32_t e = 0; e < n && e < 20000 && entries && strings; ++e) {
          const uint32_t id = Load32(base, entries + e * 32), off = Load32(base, entries + e * 32 + 12);
          std::string text;
          for (uint32_t c = 0; c < 400; ++c) {
            const uint8_t ch = base[strings + off + c];
            if (!ch) break;
            text += ch == '\n' || ch == '\r' ? ' ' : char(ch);
          }
          std::fprintf(f, "%08X %08X %s\n", Load32(base, file + 20), id, text.c_str());
        }
      }
      std::fclose(f);
      REXLOG_INFO("KK dev jump: wrote the game's texts to {}", path);
    }
  }
  // KK_DEV_START_END=<seconds>: that long after the start request's level loaded, ask for the next
  // chapter's level, as reaching the chapter's end does (uni_go then handles it as the end of a
  // chapter-select run, or goes on with the story).
  if (static bool ended = false; !ended && std::getenv("KK_DEV_START_END") && *std::getenv("KK_DEV_START_END") &&
                                 Load32(base, g + 4) == 0 &&
                                 since > std::chrono::duration<double>(std::atof(std::getenv("KK_DEV_START_END")))) {
    ended = true;
    const int next = int(Load32(base, g + 0x5670)) + 1;  // the chapter-select run's entry + 1
    Store32(base, g + 8, Load32(base, g + (0x1402 + next) * 4));
    Store32(base, g + 0xC, Load32(base, g + (0x1435 + next) * 4));
    Store32(base, g + 4, 1);
    REXLOG_INFO("KK dev jump: asking for the next chapter's level (entry {}): {}", next, Vars(base));
  }
}
}  // namespace kk

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

// KK_DEV_SAVE_TRACE=1: log the save system's functions (the ones that use
// "KKProfileSlot%d.sav" and their dispatchers) and the universe snapshot /
// restore / re-init, with the save-related universe variables.
#if defined(KK_DEV_TOOLS)
namespace kk::jump {
namespace {
bool SaveTracing() {
  static const bool on = std::getenv("KK_DEV_SAVE_TRACE") != nullptr;
  return on;
}
std::string SaveVars(const uint8_t* base) {
  const uint32_t ai = Load32(base, kAiPtr);
  const uint32_t g = ai ? Load32(base, ai + 0x40) : 0;
  const uint32_t gp = Load32(base, kGamePtr);
  const uint32_t u = gp ? gp : 0;
  if (!u) return fmt::format("G=0 (ai vars {:08X})", g);
  const uint32_t s2 = Load32(base, 0x82CC8758);
  const std::string other = s2 ? fmt::format(" | *(0x82CC8758)={:08X} +8 {} +C {}", s2, int32_t(Load32(base, s2 + 8)),
                                             int32_t(Load32(base, s2 + 0xC)))
                               : std::string(" | *(0x82CC8758)=0");
  const std::string hdr = fmt::format(" | header0 level {} start {} name {:02X}", int32_t(Load32(base, 0x82C6399C + 0x28)),
                                      int32_t(Load32(base, 0x82C6399C + 0x2C)), base[0x82C6399C]);
  return fmt::format("slot {} 4e14 {} player {} {} dev {} 5f24 {} flags {:08X} reached {} continue {} {} mode {} | user {:02X} {}",
                     int32_t(Load32(base, u + 0x4E10)), int32_t(Load32(base, u + 0x4E14)),
                     int32_t(Load32(base, u + 0x5F00)), int32_t(Load32(base, u + 0x5F04)),
                     int32_t(Load32(base, u + 0x5F08)), int32_t(Load32(base, u + 0x5F24)), Load32(base, u + 0xBF0),
                     int32_t(Load32(base, u + 0x4FF4)), int32_t(Load32(base, u + 0x4FF8)),
                     int32_t(Load32(base, u + 0x4FFC)), int32_t(Load32(base, u + 0x5668)), base[0x8308FE18 + 0x3B],
                     int32_t(Load32(base, 0x8308FE18 + 0x3C))) + other + hdr;
}
}  // namespace
}  // namespace kk::jump

#define KK_SAVE_HOOK(addr, name)                                                                            \
  REX_EXTERN(__imp__sub_##addr);                                                                            \
  REX_HOOK_RAW(sub_##addr) {                                                                                \
    using namespace kk::jump;                                                                               \
    const bool on = SaveTracing();                                                                          \
    if (on)                                                                                                 \
      REXLOG_INFO("KK dev save: {} in r3={:08X} r4={:08X} r5={:08X} r6={:08X} lr={:08X} | {}", name,         \
                  ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr), SaveVars(base));        \
    __imp__sub_##addr(ctx, base);                                                                           \
    if (on) REXLOG_INFO("KK dev save: {} out r3={:08X} | {}", name, ctx.r3.u32, SaveVars(base));            \
  }

KK_SAVE_HOOK(82721970, "slot 82721970 (XamContentDelete)")
KK_SAVE_HOOK(82721B70, "slot 82721B70 (enumerate)")
KK_SAVE_HOOK(82721DC8, "slot 82721DC8 (read?)")
KK_SAVE_HOOK(827223E8, "slot 827223E8 (write?)")
// The save system's jobs (run on a worker thread): reading every slot's save
// (the save menu opening) and loading the chosen slot's into M+0x550.
REX_EXTERN(__imp__sub_82722150);
REX_HOOK_RAW(sub_82722150) {
  using namespace kk::jump;
  if (SaveTracing()) REXLOG_INFO("KK dev save: read-all-slots job 82722150 in | {}", SaveVars(base));
  __imp__sub_82722150(ctx, base);
  if (SaveTracing()) REXLOG_INFO("KK dev save: read-all-slots job 82722150 out r3={} | {}", ctx.r3.u32, SaveVars(base));
  g_read_all_done = true;
}
REX_EXTERN(__imp__sub_82722320);
REX_HOOK_RAW(sub_82722320) {
  using namespace kk::jump;
  if (SaveTracing()) REXLOG_INFO("KK dev save: load-slot job 82722320 in (slot {}) | {}", int32_t(Load32(base, 0x82C63E9C)), SaveVars(base));
  __imp__sub_82722320(ctx, base);
  if (SaveTracing()) REXLOG_INFO("KK dev save: load-slot job 82722320 out r3={} | {}", ctx.r3.u32, SaveVars(base));
  g_load_slot_result = int(ctx.r3.u32);
  g_load_slot_done = true;
}
KK_SAVE_HOOK(82413978, "82413978")
KK_SAVE_HOOK(82413498, "restore 82413498")
KK_SAVE_HOOK(827291A0, "memcard command 827291A0")
KK_SAVE_HOOK(821075F0, "XamUserGetSigninState")
#endif
