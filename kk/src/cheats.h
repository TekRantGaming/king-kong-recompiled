// Cheats chosen on the launcher's Cheats page (kk_cheats + kk_cheat_<id>).

#pragma once

#include <array>

namespace kk {

struct CheatInfo {
  const char* id;     // kk_cheat_<id>
  const char* label;  // shown in the launcher
  const char* code;   // the game's own code for it, shown for reference
};

constexpr std::array<CheatInfo, 10> kCheats = {{
    {"chapters", "All chapters", "KKst0ry"},
    {"bonus", "All bonus content", "KKmuseum"},
    {"healing", "Fast healing (Jack)", "8wonder"},
    {"one_hit", "One-hit kills (bullets)", "GrosBras"},
    {"ammo", "999 bullets", "KK 999 mun"},
    {"spears", "Unlimited spears", "lance 1nf"},
    {"revolver", "Revolver", "KKtigun"},
    {"machine_gun", "Machine gun", "KKcapone"},
    {"shotgun", "Shotgun", "KKsh0tgun"},
    {"sniper", "Sniper rifle", "KKsn1per"},
}};

}  // namespace kk
