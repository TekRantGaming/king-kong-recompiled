// Cheats chosen on the launcher's Cheats page (kk_cheats + kk_cheat_<id>).

#pragma once

#include <array>
#include <string_view>

namespace kk {

struct CheatInfo {
  const char* id;     // kk_cheat_<id>
  const char* label;  // shown in the launcher
  const char* code;   // the game's own code for it, shown for reference
};

// "All bonus content" (KKmuseum) is not offered any more: every bonus is unlocked for everyone (extras.h,
// owner 18 Oct 2026), so its flag is always on (cheats.cpp, UnlockBonusContent). kk_cheat_bonus is ignored.
constexpr std::array<CheatInfo, 8> kCheats = {{
    {"healing", "Fast healing (Jack)", "8wonder"},
    {"one_hit", "One-hit kills (bullets)", "GrosBras"},
    {"ammo", "999 bullets", "KK 999 mun"},
    {"spears", "Unlimited spears", "lance 1nf"},
    {"revolver", "Revolver", "KKtigun"},
    {"machine_gun", "Machine gun", "KKcapone"},
    {"shotgun", "Shotgun", "KKsh0tgun"},
    {"sniper", "Sniper rifle", "KKsn1per"},
}};

// True while the level being played switches cheat `id` off (cheat_rules.h: the weapon cheats in the Hayes
// chapter). The pause menu's Cheats page greys its row then; the setting itself is not changed.
bool CheatSuppressedNow(std::string_view id);

}  // namespace kk
