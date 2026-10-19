// Cheats chosen on the launcher's Cheats page, switched on when the game
// reaches its start menu, exactly as if their codes had been typed.
//
// The game checks codes in its Cheat page (IntMIG_Page_CheatFinal,
// sub_824C1A48): the typed letters are packed 6 bits each (A-Z 0-25, a-z
// 26-51, 0-9 52-61, space 0, blank 63), five to a word, and compared with ten
// two-word constants. Each match changes the game state G = *(0x82CC97C0) or
// the profile P = *(0x82CC8D24) (the same structure in our tests):
//   8wonder     G.flags (+3056) |= 0x10000200   fast healing
//   GrosBras    G.flags |= 0x10000002           one-hit kills
//   lance 1nf   G.flags |= 0x10002000           unlimited spears
//   KK 999 mun  G ammo (+13432, +13504, +13576, +13648) = 999, G.flags |= 0x10000000
//   KKtigun     G weapon (+13128) = 1, ammo[0] >= 80, G.flags |= 0x10000000
//   KKcapone    weapon 2, ammo[1] >= 500       KKsh0tgun  weapon 3, ammo[2] >= 50
//   KKsn1per    weapon 4, ammo[3] >= 50
//   KKmuseum    P.flags (+3056) toggles 0x100
// (KKst0ry, all chapters, is not offered: the launcher's chapter select opens every chapter.)
// (KKmuseum, all bonus content, is not offered either: its flag is always on, see UnlockBonusContent.)
//
// The game's "cheats used" mark is G.flags 0x10000000, set by the gameplay codes only (fast healing,
// one-hit kills, spears, ammo, weapons; never by KKst0ry or KKmuseum). It is read in two places: at the end
// of a chapter-select run IntMIG_wait (sub_824B44E8, 0x824B4808) shows message 102 instead of recording the
// run's score, and the pause page's Score item (sub_824BA560, 0x824BB0BC) shows message 103 instead of the
// score page. G.flags is not in the save (the save descriptor has no G+0xBF0), so neither mark nor the
// unlock flag ever reaches a save file.
// (The names come from the AI function table at 0x828C9000: function, name,
// key per entry.) The port makes the same changes, setting rather than
// toggling, once the start menu page (IntMIG_Page_StartMain, sub_824BB670)
// runs (on each visit): by then the profile is loaded, and it is where the
// codes are entered. A start from the launcher's Play menu never shows that
// page: play_session.cpp applies them once the chapter's level is up
// (play::ApplyCheatsNow). Changes made in the pause menu apply at once
// (WatchChanges): a flag cheat switched off is cleared again; weapons and ammo
// given stay until the next chapter or checkpoint.
//
// Starting a chapter (or a checkpoint) empties G's weapon and ammo, then gives
// the chapter's own loadout (V-Rex: the machine gun with 30 rounds), which
// would undo the weapon and ammo cheats. So the port watches for that from the
// game's input poll (sub_8272C320, every frame) and gives them again once the
// loadout is in.
//
// Some cheats are off in some levels (cheat_rules.h; owner 19 Oct 2026: the weapon cheats in the Hayes
// chapter, level 303, and from Beta 12 the two chapters before it, 301 and 302): there they are never given, at the chapter start, after a checkpoint or when switched
// on in play, so the chapter's own loadout is what the player has. The settings are not changed.

#include "cheats.h"
#include "coop_runtime.h"
#include "cheat_rules.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "play_internal.h"
#include "settings.h"
#if defined(KK_DEV_TOOLS)
#include "coop_pose.h"
#endif

namespace kk::hardcore {
void Frame(uint8_t* base);  // hardcore.cpp: every frame (the input poll)
}

namespace kk {
namespace {

constexpr uint32_t kGamePtr = 0x82CC97C0, kProfilePtr = 0x82CC8D24;
constexpr uint32_t kFlags = 3056, kWeapon = 13128;
constexpr uint32_t kAmmo[4] = {13432, 13504, 13576, 13648};
constexpr uint32_t kCheatsUsed = 0x10000000;

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return std::byteswap(v);
}

void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
  v = std::byteswap(v);
  std::memcpy(base + addr, &v, 4);
}

}  // namespace
}  // namespace kk

REXCVAR_DECLARE(bool, kk_cheat_healing);
REXCVAR_DECLARE(bool, kk_cheat_one_hit);
REXCVAR_DECLARE(bool, kk_cheat_ammo);
REXCVAR_DECLARE(bool, kk_cheat_spears);
REXCVAR_DECLARE(bool, kk_cheat_revolver);
REXCVAR_DECLARE(bool, kk_cheat_machine_gun);
REXCVAR_DECLARE(bool, kk_cheat_shotgun);
REXCVAR_DECLARE(bool, kk_cheat_sniper);

namespace kk {
namespace {

// kk_cheat_<id>. Task 288: read straight from the cvars (settings.cpp), as REXCVAR_GET(kk_cheats) is: it runs
// every frame from the input poll (ChosenBits), and the by-name lookup built a string and took the cvar
// registry's lock (shared with the launcher and pause menu UI) 8 times a frame. Unknown ids are false, as before.
bool OwnChosen(std::string_view id) {
  if (id == "healing") return REXCVAR_GET(kk_cheat_healing);
  if (id == "one_hit") return REXCVAR_GET(kk_cheat_one_hit);
  if (id == "ammo") return REXCVAR_GET(kk_cheat_ammo);
  if (id == "spears") return REXCVAR_GET(kk_cheat_spears);
  if (id == "revolver") return REXCVAR_GET(kk_cheat_revolver);
  if (id == "machine_gun") return REXCVAR_GET(kk_cheat_machine_gun);
  if (id == "shotgun") return REXCVAR_GET(kk_cheat_shotgun);
  if (id == "sniper") return REXCVAR_GET(kk_cheat_sniper);
  return false;
}
coop::CheatOverride g_host_cheats;
bool Chosen(std::string_view id) {
  const auto host = g_host_cheats.Snapshot();
  if (!host) return OwnChosen(id);
  const auto set = *host;
  for (unsigned i = 0; i < kCheats.size(); ++i) if (id == kCheats[i].id) return set.Chosen(i);
  return false;
}
bool CheatsEnabled() {
  const auto host = g_host_cheats.Snapshot();
  return host ? host->Enabled() : REXCVAR_GET(kk_cheats);
}

// The level key last loaded (G+8): set by the game when a level load is asked for, so it is already the new
// level when that level's loadout arrives. 0 before the game is set up.
uint32_t Level(const uint8_t* base, uint32_t g) { return g ? Load32(base, g + 8) : 0; }

// The level the input poll saw last, for the pause menu (CheatSuppressedNow).
std::atomic<uint32_t> g_level{0};

// Chosen and not switched off in this level (cheat_rules.h).
// Co-op test aid (dev builds only): KK_DEV_CHEATS_ANYWHERE=1 lifts the per-chapter cheat rules (cheat_rules.h),
// so a scripted test can arm Jack where the awake raptors are (Hayes). Players never get this.
bool RuleOff(std::string_view id, uint32_t level) {
#if KK_DEV_TOOLS
  static const bool anywhere = [] { const char* v = std::getenv("KK_DEV_CHEATS_ANYWHERE"); return v && *v == '1'; }();
  if (anywhere) return false;
#endif
  return cheat_rules::Suppressed(id, level);
}
bool Applies(const char* id, uint32_t level) { return Chosen(id) && !RuleOff(id, level); }

// One line when a level that switches cheats off starts or ends (whatever the player chose: the settings stay).
void WatchLevel(const uint8_t* base) {
  const uint32_t g = Load32(base, kGamePtr);
  const uint32_t level = Level(base, g);
  const uint32_t before = g_level.exchange(level);
  const cheat_rules::LevelCheatsOff *was = cheat_rules::RuleFor(before), *now = cheat_rules::RuleFor(level);
  if (was == now) return;
  if (was && !now)
    REXLOG_INFO("KK: cheats switched off in level {} ({}) apply again as set (now level {})", was->level, was->chapter,
                level);
  if (now) {
    std::string ids;
    for (std::string_view id : now->cheats) {
      if (id.empty()) continue;
      const bool chosen = Chosen(std::string(id).c_str());
      ids += fmt::format("{}{}{}", ids.empty() ? "" : ", ", id, chosen ? " (chosen)" : "");
    }
    REXLOG_INFO("KK: level {} ({}): these cheats are off in this chapter, settings unchanged: {}", now->level,
                now->chapter, ids);
  }
}

void SetFlags(uint8_t* base, uint32_t obj, uint32_t bits) { Store32(base, obj + kFlags, Load32(base, obj + kFlags) | bits); }

// Gives weapon n (1-4) with at least `ammo` rounds, as its code does.
void GiveWeapon(uint8_t* base, uint32_t g, int n, uint32_t ammo) {
  SetFlags(base, g, kCheatsUsed);
  Store32(base, g + kWeapon, uint32_t(n));
  const uint32_t at = g + kAmmo[n - 1];
  Store32(base, at, std::max(Load32(base, at), ammo));
}

bool g_applied = false;

// All bonus content is unlocked for every player (owner, 18 Oct 2026): the game's own flag for it (KKmuseum,
// profile flag 0x100) is kept on while the game runs, so its checks (the Video options' bonus modes, the
// bonus availability tests sub_825EAC88 / sub_825E8E00 and the like) all pass. Not a cheat to the game, and
// never saved (see the top of this file). Cheap enough for every frame: a universe re-init clears it.
void UnlockBonusContent(uint8_t* base) {
  const uint32_t p = Load32(base, kProfilePtr);
  if (!p || (Load32(base, p + kFlags) & 0x100)) return;
  SetFlags(base, p, 0x100);
  static bool logged = false;
  if (!logged) REXLOG_INFO("KK: all bonus content unlocked (the game's bonus flag is on; not a cheat)");
  logged = true;
}

std::string State(const uint8_t* base, uint32_t g, uint32_t p) {
  return fmt::format("game flags {:08X}, weapon {}, ammo {}/{}/{}/{}; profile flags {:08X}", Load32(base, g + kFlags),
                     Load32(base, g + kWeapon), Load32(base, g + kAmmo[0]), Load32(base, g + kAmmo[1]),
                     Load32(base, g + kAmmo[2]), Load32(base, g + kAmmo[3]), Load32(base, p + kFlags));
}

// The weapon and ammo cheats, those that apply in this level. Returns how many are switched on.
int ApplyLoadoutCheats(uint8_t* base, uint32_t g) {
  const uint32_t level = Level(base, g);
  int count = 0;
  // Weapons: the last one switched on is the one Jack holds, like typing the codes in this order.
  static constexpr struct {
    const char* id;
    int weapon;
    uint32_t ammo;
  } kWeapons[] = {{"revolver", 1, 80}, {"machine_gun", 2, 500}, {"shotgun", 3, 50}, {"sniper", 4, 50}};
  for (const auto& w : kWeapons) {
    if (!Applies(w.id, level)) continue;
    GiveWeapon(base, g, w.weapon, w.ammo);
    ++count;
  }
  if (Applies("ammo", level)) {
    for (uint32_t a : kAmmo) Store32(base, g + a, 999);
    SetFlags(base, g, kCheatsUsed);
    ++count;
  }
  return count;
}

void ApplyCheats(uint8_t* base) {
  const uint32_t g = Load32(base, kGamePtr), p = Load32(base, kProfilePtr);
  if (!g || !p) return;  // not set up yet: try again next frame
  g_applied = true;
  const std::string before = State(base, g, p);
  int count = 0;
  auto on = [&](const char* id) {
    const bool chosen = Applies(id, Level(base, g));
    count += chosen;
    return chosen;
  };
#if defined(KK_DEV_TOOLS)
  // The golden-frame harness navigates the original chapter-select page.
  // Keep its unlock out of the player's cheats and release builds.
  if (const char* chapters = std::getenv("KK_DEV_CHAPTERS"); chapters && std::strcmp(chapters, "1") == 0) {
    SetFlags(base, p, 0x80);
    ++count;
  }
#endif
  if (on("healing")) SetFlags(base, g, kCheatsUsed | 0x200);
  if (on("one_hit")) SetFlags(base, g, kCheatsUsed | 0x2);
  if (on("spears")) SetFlags(base, g, kCheatsUsed | 0x2000);
  count += ApplyLoadoutCheats(base, g);
  REXLOG_INFO("KK: cheats on ({} chosen). Before: {}. After: {}", count, before, State(base, g, p));
}

// After a chapter or checkpoint starts: G's weapon and ammo go to 0, then the
// chapter's loadout arrives. Give the weapon and ammo cheats again once the
// loadout has been in place for a second (so all of it has arrived).
bool g_loadout_cleared = false;
std::chrono::steady_clock::time_point g_loadout_seen{};

#if defined(KK_DEV_TOOLS)
// A synthetic checkpoint can clear the early cheat grant and never supply a
// nonempty authored loadout. Keep the normal player's checkpoint watcher intact:
// only KK_DEV_START_POINT opts into this empty-but-playable fallback.
bool DevEmptyLoadoutReady(const uint8_t* base, bool empty) {
  static const bool enabled = [] {
    const char* value = std::getenv("KK_DEV_START_POINT");
    if (!value || !*value) return false;
    char* end = nullptr;
    const auto start = std::strtol(value, &end, 10);
    return end != value && !*end && start >= 0 && start <= 10000;
  }();
  if (!enabled) return false;
  static std::chrono::steady_clock::time_point since{};
  static uint32_t previous_game = 0, previous_actor = 0;
  if (!empty || !g_applied || play::StartLevelLoading()) {
    since = {};
    return false;
  }
  const uint32_t game = Load32(base, kGamePtr), actor = Load32(base, 0x82CCBF7C);
  const auto pose = coop::ReadJackPose([&](uint32_t p) { return Load32(base, p); },
      [&](uint32_t p) { return uint16_t(Load32(base, p & ~3u) >> ((p & 2u) ? 0 : 16)); },
      [&](uint32_t p) { return std::bit_cast<float>(Load32(base, p)); }, 0);
  if (!pose.pose || game != previous_game || actor != previous_actor) since = {};
  previous_game = game; previous_actor = actor;
  if (!pose.pose) return false;
  const auto now = std::chrono::steady_clock::now();
  if (since == std::chrono::steady_clock::time_point{}) since = now;
  if (now - since < std::chrono::seconds(1)) return false;
  since = {};
  return true;
}
#endif

void WatchLoadout(uint8_t* base) {
  const uint32_t g = Load32(base, kGamePtr), p = Load32(base, kProfilePtr);
  if (!g || !p) return;
  bool empty = Load32(base, g + kWeapon) == 0;
  for (uint32_t a : kAmmo) empty = empty && Load32(base, g + a) == 0;
  const auto now = std::chrono::steady_clock::now();
#if defined(KK_DEV_TOOLS)
  if (DevEmptyLoadoutReady(base, empty)) {
    const std::string before = State(base, g, p);
    const int count = ApplyLoadoutCheats(base, g);
    g_loadout_cleared = false;
    g_loadout_seen = {};
    REXLOG_INFO("KK dev: synthetic start stayed playable with an empty loadout for 1 s; re-applied {} loadout cheats. Before: {}. After: {}",
                count, before, State(base, g, p));
    return;
  }
#endif
  if (empty) {
    g_loadout_cleared = true;
    g_loadout_seen = {};
    return;
  }
  if (!g_loadout_cleared) return;
  if (g_loadout_seen == std::chrono::steady_clock::time_point{}) g_loadout_seen = now;
  if (now - g_loadout_seen < std::chrono::seconds(1)) return;
  g_loadout_cleared = false;
  const std::string before = State(base, g, p);
  if (ApplyLoadoutCheats(base, g) > 0)
    REXLOG_INFO("KK: weapon and ammo cheats given again for the new loadout. Before: {}. After: {}", before,
                State(base, g, p));
}

// The cheats chosen now, as bits (kCheats order), to see changes made in the pause menu.
uint32_t ChosenBits() {
  const auto set = EffectiveCheats();
  uint32_t bits = set.Enabled() ? 1u << 31 : 0;
  for (size_t i = 0; i < kCheats.size(); ++i)
    if (set.Chosen(unsigned(i))) bits |= 1u << i;
  return bits;
}
uint32_t g_applied_bits = 0;

// Settings changed while playing (the pause menu): switch on what was added, and clear the flag cheats
// that were switched off (weapons and ammo already given stay until the next loadout).
void WatchChanges(uint8_t* base) {
  if (!g_applied) return;
  const uint32_t bits = ChosenBits();
  if (bits == g_applied_bits) return;
  const uint32_t g = Load32(base, kGamePtr), p = Load32(base, kProfilePtr);
  if (!g || !p) return;
  const std::string before = State(base, g, p);
  const bool all_on = CheatsEnabled();
  auto clear = [&](uint32_t obj, uint32_t bit) { Store32(base, obj + kFlags, Load32(base, obj + kFlags) & ~bit); };
  struct Flag {
    const char* id;
    bool profile;
    uint32_t bit;
  };
  static constexpr Flag kFlagCheats[] = {{"healing", false, 0x200}, {"one_hit", false, 0x2}, {"spears", false, 0x2000}};
  auto was_on = [&](const char* id) {
    for (size_t i = 0; i < kCheats.size(); ++i)
      if (std::string(kCheats[i].id) == id) return (g_applied_bits & (1u << 31)) && (g_applied_bits & (1u << i));
    return false;
  };
  for (const Flag& f : kFlagCheats) {
    const bool want = all_on && Applies(f.id, Level(base, g));
    const uint32_t obj = f.profile ? p : g;
    if (want) SetFlags(base, obj, f.profile ? f.bit : (kCheatsUsed | f.bit));
    else if (was_on(f.id)) clear(obj, f.bit);  // only what the port switched on
  }
  if (all_on) {
    // Weapons and ammo newly chosen: given now (unless this level switches them off).
    for (size_t i = 0; i < kCheats.size(); ++i) {
      if (!(bits & (1u << i)) || (g_applied_bits & (1u << 31) && g_applied_bits & (1u << i))) continue;
      if (RuleOff(kCheats[i].id, Level(base, g))) continue;  // off in this level (cheat_rules.h)
      const std::string id = kCheats[i].id;
      if (id == "revolver") GiveWeapon(base, g, 1, 80);
      else if (id == "machine_gun") GiveWeapon(base, g, 2, 500);
      else if (id == "shotgun") GiveWeapon(base, g, 3, 50);
      else if (id == "sniper") GiveWeapon(base, g, 4, 50);
      else if (id == "ammo") {
        for (uint32_t a : kAmmo) Store32(base, g + a, 999);
        SetFlags(base, g, kCheatsUsed);
      }
    }
  }
  g_applied_bits = bits;
  REXLOG_INFO("KK: cheats changed in play. Before: {}. After: {}", before, State(base, g, p));
}

#if defined(KK_DEV_TOOLS)
// Sustained-firing tests outlast the 999-bullet loadout. Only this opt-in dev aid
// replenishes it, and logs consumption so a held trigger is not mistaken for firing.
void DevGunfire(uint8_t* base) {
  static const bool enabled = [] {
    const char* value = std::getenv("KK_DEV_GUNFIRE");
    return value && *value == '1';
  }();
  if (!enabled || !CheatsEnabled() || !Chosen("ammo") || !Chosen("machine_gun")) return;
  const uint32_t g = Load32(base, kGamePtr);
  if (!g || Load32(base, g + kWeapon) != 2) return;
  const uint32_t ammo = Load32(base, g + kAmmo[1]);
  static uint32_t previous = 0;
  static uint64_t consumed = 0;
  if (previous > ammo && previous - ammo < 100) consumed += previous - ammo;
  previous = ammo;
  if (ammo < 300) {
    Store32(base, g + kAmmo[1], 999);
    previous = 999;
    REXLOG_INFO("KK dev gunfire: replenished machine-gun ammo {} to 999, {} consumed", ammo, consumed);
  }
  static auto last = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  if (now - last >= std::chrono::seconds(10)) {
    last = now;
    REXLOG_INFO("KK dev gunfire: machine-gun ammo {}, {} consumed", ammo, consumed);
  }
}
#endif

}  // namespace
}  // namespace kk

namespace kk {
bool CheatSuppressedNow(std::string_view id) { return RuleOff(id, g_level.load()); }
coop::CheatSet OwnCheats() {
  static_assert(kCheats.size() == 8, "Update the co-op cheat codec when adding gameplay cheats");
  coop::CheatSet set{uint16_t(REXCVAR_GET(kk_cheats) ? 0x100 : 0)};
  for (unsigned i = 0; i < kCheats.size(); ++i) if (OwnChosen(kCheats[i].id)) set.bits |= uint16_t(1u << i);
  return set;
}
coop::CheatSet EffectiveCheats() {
  const auto host = g_host_cheats.Snapshot();
  // Avoid all own-setting reads while host control is active.
  return coop::SessionCheats(host ? coop::CheatSet{} : OwnCheats(), host);
}
bool CheatsSetByHost() { return g_host_cheats.Snapshot().has_value(); }
void SetSessionCheats(std::optional<coop::CheatSet> host) {
  if (g_host_cheats.Snapshot() == host || !g_host_cheats.Apply(host)) return;
  REXLOG_INFO("KK co-op cheats: source={} bits={:03X} own-settings-unchanged=true",
      host ? "host" : "own", EffectiveCheats().bits);
}
}  // namespace kk

namespace kk::play {
bool ApplyCheatsNow(uint8_t* base) {
  coop::RefreshSessionCheats();
  const uint32_t g = Load32(base, kGamePtr), p = Load32(base, kProfilePtr);
  if (!g || !p) return false;
  kk::UnlockBonusContent(base);
  if (kk::CheatsEnabled()) kk::ApplyCheats(base);
  // For player logs and the scoring check (task 204): the game's own marks at the level start.
  const uint32_t flags = Load32(base, g + kFlags);
  REXLOG_INFO("KK: game flags {:08X} at the level start: cheats-used mark {}, all bonus content {}", flags,
              (flags & kCheatsUsed) ? "ON (scores of this run are not recorded)" : "off", (flags & 0x100) ? "on" : "off");
  kk::g_applied = true;
  kk::g_applied_bits = kk::ChosenBits();
  return true;
}
}  // namespace kk::play

// IntMIG_Page_StartMain: the start menu page, run every frame while it shows.
// Applied again each time the menu comes back (after quitting a level or
// loading another save), as typing the codes again would.
REX_EXTERN(__imp__sub_824BB670);
REX_HOOK_RAW(sub_824BB670) {
  if (kk::play::OnGameMenuPage("IntMIG_Page_StartMain")) return;  // never shown after a launcher start
  static auto last = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  if (now - last > std::chrono::seconds(2)) kk::g_applied = false;  // a new visit to the menu
  last = now;
  kk::UnlockBonusContent(base);
  if (!kk::g_applied && kk::CheatsEnabled()) {
    kk::ApplyCheats(base);
    kk::g_applied_bits = kk::ChosenBits();
  }
  __imp__sub_824BB670(ctx, base);
}

// The game's input poll, every frame: watch for a chapter's new loadout.
REX_EXTERN(__imp__sub_8272C320);
REX_HOOK_RAW(sub_8272C320) {
  __imp__sub_8272C320(ctx, base);
  kk::coop::RefreshSessionCheats();
  kk::UnlockBonusContent(base);
  kk::WatchLevel(base);
  if (kk::CheatsEnabled()) kk::WatchLoadout(base);
  kk::WatchChanges(base);
  kk::hardcore::Frame(base);
#if defined(KK_DEV_TOOLS)
  kk::DevGunfire(base);
#endif
}
