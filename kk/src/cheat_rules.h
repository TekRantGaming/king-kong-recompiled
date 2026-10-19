// Cheats that are off in some chapters whatever the player chose (owner, 19 Oct 2026; task 251).
//
// The weapon cheats could leave the Hayes chapter's raptor fight stuck (codex\state\hayes-dino.md), so in
// that level they stop applying: no weapon is given when the chapter or a checkpoint starts (the chapter's own
// loadout is what the player has), and the pause menu's Cheats page greys their rows. The player's settings
// (kk_cheat_<id>) are never changed: in any other level the cheats apply again as before.
//
// One small table: to switch off another cheat there (e.g. "ammo", 999 bullets) add its kCheats id (cheats.h)
// to the level's list; another level is another line. No game or ImGui needed (tests/cheat_rules_tests.cpp).
// This suits the cheats the game takes away at each chapter or checkpoint start (weapons, ammo). A flag cheat
// (healing, one-hit kills, spears) already on when the level starts would stay on: cheats.cpp would also have to
// clear it on entering the level.

#pragma once

#include <cstdint>
#include <string_view>

namespace kk::cheat_rules {

struct LevelCheatsOff {
  uint32_t level;               // the game's level key (G+8, G = [0x82CC97C0]); chapter select entry in the comment
  const char* chapter;          // for the log
  std::string_view cheats[8];   // kCheats ids (cheats.h); unused slots stay empty
};

inline constexpr LevelCheatsOff kLevelCheatsOff[] = {
    {303, "Hayes", {"revolver", "machine_gun", "shotgun", "sniper"}},  // chapter select entry 7
};

// The note on a greyed row in the pause menu (5 languages: pause_text_table.inc).
inline constexpr const char* kNotInThisChapter = "Not available in this chapter.";

// The rule for `level`, or nullptr when every cheat applies there.
constexpr const LevelCheatsOff* RuleFor(uint32_t level) {
  for (const LevelCheatsOff& r : kLevelCheatsOff)
    if (r.level == level) return &r;
  return nullptr;
}

// True when cheat `id` must not apply in `level`.
constexpr bool Suppressed(std::string_view id, uint32_t level) {
  const LevelCheatsOff* r = RuleFor(level);
  if (!r || id.empty()) return false;
  for (std::string_view c : r->cheats)
    if (c == id) return true;
  return false;
}

}  // namespace kk::cheat_rules
