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
int g_frames_ready = 0;

}  // namespace
}  // namespace kk::jump

REX_EXTERN(__imp__sub_82413190);
REX_EXTERN(__imp__sub_8271D698);
REX_EXTERN(__imp__sub_82413080);
REX_EXTERN(__imp__sub_82700A48);
REX_EXTERN(__imp__sub_827291A0);
REX_EXTERN(__imp__sub_82729138);
REX_EXTERN(__imp__sub_82721618);

namespace kk::jump {
namespace {

// What pressing A on chapter-select entry `idx` does (IntMIG_Page_Map,
// sub_824BD150, the branch for a chapter other than the one in progress),
// followed by what the load page (IntMIG_Page_load, sub_824B4DF0) does with
// it: ask the universe for the chapter's level.
// The save system (memcard object M at 0x82C63950): M+0x4C holds the five
// slots' 0x100-byte headers, M+0x550 (0x82C63EA0) the current save's 0x7F40
// bytes of data (the universe's persistent variables, then 0x2F40 bytes of the
// block at 0x82CC5568), M+0x54C the current slot. A slot's file
// (KKProfileSlot<n+1>.sav/KKProfile.dat) is the header then the data.
// sub_82413190 copies the universe into the data, sub_82413080 copies it back.
constexpr uint32_t kMemcard = 0x82C63950, kSaveHeaders = kMemcard + 0x4C, kSaveData = kMemcard + 0x550,
                   kSaveSlot = kMemcard + 0x54C;

// KK_DEV_JUMP_SAVE=<slot> with KK_DEV_JUMP_SAVEFILE=<that slot's KKProfile.dat>:
// load the save before the jump, as choosing it in the save menu does, and
// register pad 1's player as Press Start does. Returns false if the file is
// not a save.
bool LoadSave(PPCContext& ctx, uint8_t* base, uint32_t g, int slot, const char* file) {
  std::vector<uint8_t> data;
  if (FILE* f = std::fopen(file, "rb")) {
    data.resize(0x8040);
    data.resize(std::fread(data.data(), 1, data.size(), f));
    std::fclose(f);
  }
  if (data.size() != 0x8040) {
    REXLOG_WARN("KK dev jump: {} is not a save ({} bytes)", file, data.size());
    return false;
  }
  const PPCContext saved = ctx;
  // Press Start: the active player is the pad that pressed it (*(0x82C63704)).
  Store32(base, 0x82C63704, 0);
  __imp__sub_82700A48(ctx, base);
  ctx = saved;
  // The save system's buffer of all five slots' files (M+0x14, allocated and
  // cleared by sub_82729138 as the save menu does): the save job copies the
  // current slot's header and data into it before writing the file, so it
  // must exist (else that copy lands at guest address 0).
  Store32(base, kSaveSlot, uint32_t(slot));
  __imp__sub_82729138(ctx, base);
  ctx = saved;
  const uint32_t all = Load32(base, kMemcard + 0x14);
  if (all) std::memcpy(base + all + uint32_t(slot) * 0x8040, data.data(), 0x8040);
  std::memcpy(base + kSaveHeaders + uint32_t(slot) * 0x100, data.data(), 0x100);
  std::memcpy(base + kSaveData, data.data() + 0x100, 0x7F40);
  __imp__sub_82413080(ctx, base);  // the save's variables into the universe
  ctx = saved;
  Store32(base, g + 0x4E10, uint32_t(slot));  // the save menu page: the chosen slot
  Store32(base, 0x828E3898, 1);               // the storage device the device selector chose
  Store32(base, g + 0x5F08, 1);               // ... as the universe saw it
  // The save's continue point is in its header (+0x28 level, +0x2C start).
  Store32(base, g + 0x4FF8, Load32(data.data(), 0x28));
  Store32(base, g + 0x4FFC, Load32(data.data(), 0x2C));
  Store32(base, g + 0x5F24, 0);
  REXLOG_INFO("KK dev jump: loaded save slot {} from {} (slot buffer M+0x14 = {:08X}): {}", slot, file,
              Load32(base, kMemcard + 0x14), Vars(base));
  return true;
}

// KK_DEV_JUMP_SAVE=<slot> without KK_DEV_JUMP_SAVEFILE: load the save the
// game's own way, driving its save system (sub_827291A0 / the handler at
// M+0xC, sub_82721618) with the commands the save menu page sends, on the
// title screen, over several frames. Returns true when done (or given up).
bool DriveSaveLoad(PPCContext& ctx, uint8_t* base, uint32_t g, int slot) {
  static int phase = 0;
  static std::chrono::steady_clock::time_point since{};
  const auto now = std::chrono::steady_clock::now();
  const PPCContext saved = ctx;
  auto send = [&](bool via_handler, uint32_t cmd, uint32_t arg) {
    ctx.r3.u64 = cmd;
    ctx.r4.u64 = arg;
    if (via_handler) __imp__sub_82721618(ctx, base);
    else __imp__sub_827291A0(ctx, base);
    ctx = saved;
  };
  if (phase == 0) {
    Store32(base, 0x82C63704, 0);  // Press Start: pad 1's player becomes the active one
    __imp__sub_82700A48(ctx, base);
    ctx = saved;
    send(false, 0x20000070, 0);  // what the save menu page sends as it opens
    send(true, 0x20000080, 0);
    send(true, 0x200000C0, 0);
    REXLOG_INFO("KK dev jump: asked the save system to read the save slots");
    phase = 1;
    since = now;
    return false;
  }
  if (phase == 1) {
    // Wait for the job to finish and the save system to go idle again
    // (status 0x10000000): a command sent while it is busy is ignored.
    if (!g_read_all_done || Load32(base, 0x82C8E7D0) != 0x10000000) {
      if (now - since < std::chrono::seconds(20)) return false;
      REXLOG_WARN("KK dev jump: the save slots were not read in 20 s");
      return true;
    }
    REXLOG_INFO("KK dev jump: save slots read (status {:08X}, slot buffer {:08X}); loading slot {}",
                Load32(base, 0x82C8E7D0), Load32(base, kMemcard + 0x14), slot);
    // 0x20000050 = load the slot (job type 2). (0x20000060 is SAVE to the slot:
    // sent here by mistake once, it overwrote the private test save.)
    send(false, 0x20000050, uint32_t(slot));  // choosing the slot in the save menu
    phase = 2;
    since = now;
    return false;
  }
  if (phase == 2) {
    if (!g_load_slot_done) {
      if (now - since < std::chrono::seconds(20)) return false;
      REXLOG_WARN("KK dev jump: slot {} was not loaded in 20 s", slot);
      return true;
    }
    if (g_load_slot_result != 1) {
      REXLOG_WARN("KK dev jump: loading slot {} failed ({})", slot, int(g_load_slot_result));
      return true;
    }
    __imp__sub_82413080(ctx, base);  // the save's variables into the universe
    ctx = saved;
    const uint32_t header = kSaveHeaders + uint32_t(slot) * 0x100;
    Store32(base, g + 0x4E10, uint32_t(slot));
    if (int32_t(Load32(base, 0x828E3898)) < 0) Store32(base, 0x828E3898, 1);
    Store32(base, g + 0x5F08, Load32(base, 0x828E3898));
    Store32(base, g + 0x5F24, 0);
    Store32(base, g + 0x4FF8, Load32(base, header + 0x28));
    Store32(base, g + 0x4FFC, Load32(base, header + 0x2C));
    REXLOG_INFO("KK dev jump: loaded save slot {} the game's way: {}", slot, Vars(base));
    phase = 3;
  }
  return true;
}

void StartChapter(PPCContext& ctx, uint8_t* base, uint32_t g, int idx) {
  const PPCContext saved = ctx;
  int save_slot = -1;
  if (const char* slot = std::getenv("KK_DEV_JUMP_SAVE"); slot && *slot) {
    const char* file = std::getenv("KK_DEV_JUMP_SAVEFILE");
    if (!file || !*file) save_slot = std::atoi(slot);  // loaded by DriveSaveLoad
    else if (LoadSave(ctx, base, g, std::atoi(slot), file)) save_slot = std::atoi(slot);
    ctx = saved;
  }
  // The universe re-init below resets the variables that are not saved, the
  // storage device ones among them: set them again afterwards.
  struct Restore {
    uint8_t* base;
    uint32_t g;
    int slot;
    ~Restore() {
      // KK_DEV_JUMP_FLAGS=<hex>: OR these into the profile flags (0x80 = what
      // the launcher's chapters cheat sets on the start menu, which a jump skips).
      if (const char* f = std::getenv("KK_DEV_JUMP_FLAGS"); f && *f)
        Store32(base, g + 0xBF0, Load32(base, g + 0xBF0) | uint32_t(std::strtoul(f, nullptr, 16)));
      if (slot < 0) return;
      Store32(base, g + 0x4E10, uint32_t(slot));
      Store32(base, g + 0x5F08, 1);
      Store32(base, g + 0x5F24, 0);
      // The continue point too (from the save's header): the save code builds
      // the new header from it, and a header with level 0 counts as an empty
      // slot, which makes the game DELETE the save (XamContentDelete) when it
      // saves. Found the hard way on a private copy.
      const uint32_t header = kSaveHeaders + uint32_t(slot) * 0x100;
      Store32(base, g + 0x4FF8, Load32(base, header + 0x28));
      Store32(base, g + 0x4FFC, Load32(base, header + 0x2C));
    }
  } restore{base, g, save_slot};
  // KK_DEV_JUMP_CONTINUE=1: continue the save (its continue point G+0x4FF8 /
  // G+0x4FFC, as Play on the main menu does) instead of a chapter-select run.
  if (EnvInt("KK_DEV_JUMP_CONTINUE", 0)) {
    int32_t map = int32_t(Load32(base, g + 0x4FF8)), start = int32_t(Load32(base, g + 0x4FFC));
    if (map == 100 && start == -1) start = 0;  // as the load page does
    Store32(base, g + 0x5668, 0);
    Store32(base, g + 8, uint32_t(map));
    Store32(base, g + 0xC, uint32_t(start));
    if (map == 100 && start == 0) Store32(base, g + 0x1C, 1);  // the load page: a new game
    Store32(base, 0x828E3B48, 1);
    Store32(base, g + 0x5F1C, 0);
    Store32(base, g + 4, 1);
    REXLOG_INFO("KK dev jump: continuing the save (map {}, start {}): {}", map, start, Vars(base));
    return;
  }
  if (EnvInt("KK_DEV_JUMP_SNAPSHOT", 1)) {
    __imp__sub_82413190(ctx, base);                                  // snapshot the universe variables
    std::memmove(base + 0x82C6BDE0, base + 0x82C63EA0, 0x7F40);  // the restart copy (M+0x10000-0x7B70)
    std::memmove(base + 0x82C73D20, base + 0x82C6399C, 0x500);   // and of the headers (M+0x10000+0x3D0)
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
  if (jump >= 0 && !jumped && g && (g_menu_ready || !EnvInt("KK_DEV_JUMP_MENU", 0)) && Load32(base, g + 4) == 0 &&
      ++g_frames_ready > EnvInt("KK_DEV_JUMP_WAIT", 1)) {
    const char* save = std::getenv("KK_DEV_JUMP_SAVE");
    const char* file = std::getenv("KK_DEV_JUMP_SAVEFILE");
    const bool game_way = save && *save && !(file && *file);
    if (game_way && !DriveSaveLoad(ctx, base, g, std::atoi(save))) goto run;
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
run:
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
  // (After a jump the time counts from when its level finished loading, else
  // from the chapter-select run's level request.)
  const auto end_from = jumped ? jump_loaded : loaded;
  if (static bool ended = false; !ended && end_from != std::chrono::steady_clock::time_point{} &&
                                 std::getenv("KK_DEV_JUMP_END") && Load32(base, g + 4) == 0 &&
                                 std::chrono::steady_clock::now() - end_from >
                                     std::chrono::duration<double>(std::atof(std::getenv("KK_DEV_JUMP_END")))) {
    ended = true;
    const int next = int(Load32(base, g + 0x5670)) + 1;  // the chapter-select run's entry + 1
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
KK_SAVE_HOOK(82722620, "dispatch 82722620")
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
  if (SaveTracing()) REXLOG_INFO("KK dev save: load-slot job 82722320 in (slot {}) | {}", int32_t(Load32(base, kSaveSlot)), SaveVars(base));
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

// The save routine (sub_8266AD80, mode in r3): it builds the save from the
// universe, with the continue point G+0x4FF8/0x4FFC as the header's level and
// start. KK_DEV_SAVE_TRACE logs it; KK_DEV_JUMP_GUARD=1 (after a jump that
// loaded a save) puts the save's own continue point back when it is 0 there,
// since a level of 0 makes the game delete the save instead.
#if defined(KK_DEV_TOOLS)
REX_EXTERN(__imp__sub_8266AD80);
REX_HOOK_RAW(sub_8266AD80) {
  using namespace kk::jump;
  const uint32_t g = Load32(base, 0x82CC8758);
  if (g) {
    const int32_t slot = int32_t(Load32(base, g + 0x4E10));
    if (SaveTracing())
      REXLOG_INFO("KK dev save: save routine 8266AD80 mode {} | G+8 {} G+C {} | {}", int32_t(ctx.r3.u32),
                  int32_t(Load32(base, g + 8)), int32_t(Load32(base, g + 0xC)), SaveVars(base));
    if (std::getenv("KK_DEV_JUMP_GUARD") && slot >= 0 && slot < 5 && Load32(base, g + 0x4FF8) == 0) {
      const uint32_t header = kSaveHeaders + uint32_t(slot) * 0x100;
      Store32(base, g + 0x4FF8, Load32(base, header + 0x28));
      Store32(base, g + 0x4FFC, Load32(base, header + 0x2C));
      REXLOG_INFO("KK dev jump: guard: continue point was 0, put back the save's ({} {})",
                  int32_t(Load32(base, g + 0x4FF8)), int32_t(Load32(base, g + 0x4FFC)));
    }
  }
  __imp__sub_8266AD80(ctx, base);
}
#endif
