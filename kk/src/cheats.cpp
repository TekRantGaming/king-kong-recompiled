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
//   KKmuseum    P.flags (+3056) toggles 0x100   KKst0ry    P.flags toggles 0x80
// (The names come from the AI function table at 0x828C9000: function, name,
// key per entry.) The port makes the same changes, setting rather than
// toggling, once the start menu page (IntMIG_Page_StartMain, sub_824BB670)
// runs (on each visit): by then the profile is loaded, and it is where the
// codes are entered.
//
// Starting a chapter (or a checkpoint) empties G's weapon and ammo, then gives
// the chapter's own loadout (V-Rex: the machine gun with 30 rounds), which
// would undo the weapon and ammo cheats. So the port watches for that from the
// game's input poll (sub_8272C320, every frame) and gives them again once the
// loadout is in.

#include "cheats.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "settings.h"

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

bool Chosen(const char* id) { return rex::cvar::GetFlagByName(std::string("kk_cheat_") + id) == "true"; }

void SetFlags(uint8_t* base, uint32_t obj, uint32_t bits) { Store32(base, obj + kFlags, Load32(base, obj + kFlags) | bits); }

// Gives weapon n (1-4) with at least `ammo` rounds, as its code does.
void GiveWeapon(uint8_t* base, uint32_t g, int n, uint32_t ammo) {
  SetFlags(base, g, kCheatsUsed);
  Store32(base, g + kWeapon, uint32_t(n));
  const uint32_t at = g + kAmmo[n - 1];
  Store32(base, at, std::max(Load32(base, at), ammo));
}

bool g_applied = false;

std::string State(const uint8_t* base, uint32_t g, uint32_t p) {
  return fmt::format("game flags {:08X}, weapon {}, ammo {}/{}/{}/{}; profile flags {:08X}", Load32(base, g + kFlags),
                     Load32(base, g + kWeapon), Load32(base, g + kAmmo[0]), Load32(base, g + kAmmo[1]),
                     Load32(base, g + kAmmo[2]), Load32(base, g + kAmmo[3]), Load32(base, p + kFlags));
}

// The weapon and ammo cheats. Returns how many are switched on.
int ApplyLoadoutCheats(uint8_t* base, uint32_t g) {
  int count = 0;
  // Weapons: the last one switched on is the one Jack holds, like typing the codes in this order.
  static constexpr struct {
    const char* id;
    int weapon;
    uint32_t ammo;
  } kWeapons[] = {{"revolver", 1, 80}, {"machine_gun", 2, 500}, {"shotgun", 3, 50}, {"sniper", 4, 50}};
  for (const auto& w : kWeapons) {
    if (!Chosen(w.id)) continue;
    GiveWeapon(base, g, w.weapon, w.ammo);
    ++count;
  }
  if (Chosen("ammo")) {
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
    const bool chosen = Chosen(id);
    count += chosen;
    return chosen;
  };
  if (on("chapters")) SetFlags(base, p, 0x80);
  if (on("bonus")) SetFlags(base, p, 0x100);
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

void WatchLoadout(uint8_t* base) {
  const uint32_t g = Load32(base, kGamePtr), p = Load32(base, kProfilePtr);
  if (!g || !p) return;
  bool empty = Load32(base, g + kWeapon) == 0;
  for (uint32_t a : kAmmo) empty = empty && Load32(base, g + a) == 0;
  const auto now = std::chrono::steady_clock::now();
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

}  // namespace
}  // namespace kk

// IntMIG_Page_StartMain: the start menu page, run every frame while it shows.
// Applied again each time the menu comes back (after quitting a level or
// loading another save), as typing the codes again would.
REX_EXTERN(__imp__sub_824BB670);
REX_HOOK_RAW(sub_824BB670) {
  static auto last = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  if (now - last > std::chrono::seconds(2)) kk::g_applied = false;  // a new visit to the menu
  last = now;
  if (!kk::g_applied && REXCVAR_GET(kk_cheats)) kk::ApplyCheats(base);
  __imp__sub_824BB670(ctx, base);
}

// The game's input poll, every frame: watch for a chapter's new loadout.
REX_EXTERN(__imp__sub_8272C320);
REX_HOOK_RAW(sub_8272C320) {
  __imp__sub_8272C320(ctx, base);
  if (REXCVAR_GET(kk_cheats)) kk::WatchLoadout(base);
}
