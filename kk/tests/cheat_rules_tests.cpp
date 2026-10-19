// Task 251: cheats that are off in some chapters (cheat_rules.h; owner 19 Oct 2026): the weapon cheats are off in
// the Hayes chapter (level 303) and, from Beta 12, the two chapters before it (301, 302) and nowhere else, every other cheat applies everywhere, every id in the table is a
// real cheat (cheats.h), and the pause menu's note has its 5 languages (pause_text_table.inc).
#include <cstdio>
#include <string_view>

#include "cheat_rules.h"
#include "cheats.h"

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what);
  }
}

struct TextRow {
  const char* text[5];  // English, German, Spanish, French, Italian
};
constexpr TextRow kPauseText[] = {
#include "pause_text_table.inc"
};
}  // namespace

int main() {
  using namespace kk::cheat_rules;
  constexpr std::string_view kWeapons[] = {"revolver", "machine_gun", "shotgun", "sniper"};

  // Hayes (level 303, chapter select entry 7) and the two chapters before it (Beta 12: Sacrifice 301 = entry 5,
  // On Kong's tracks 302 = entry 6), so no cheat gun is carried into Hayes: the four weapon cheats are off.
  for (uint32_t level : {301u, 302u, 303u}) {
    Check(RuleFor(level) != nullptr, "levels 301, 302 and 303 have a rule");
    for (std::string_view w : kWeapons) Check(Suppressed(w, level), "a weapon cheat is off in levels 301 to 303");
    // The other cheats are unchanged there.
    for (std::string_view other : {"healing", "one_hit", "ammo", "spears", "bonus", ""})
      Check(!Suppressed(other, level), "a cheat that is not a weapon cheat applies in levels 301 to 303");
  }
  static_assert(Suppressed("machine_gun", 302) && !Suppressed("machine_gun", 204), "usable at compile time");

  // The chapters before and after (The wall 204, V-Rex 305, 304), the start menu and "no level yet": every cheat
  // applies.
  for (uint32_t level : {0u, 204u, 304u, 305u, 300u, 3030u, 33u}) {
    Check(RuleFor(level) == nullptr, "no rule outside levels 301 to 303");
    for (const kk::CheatInfo& c : kk::kCheats) Check(!Suppressed(c.id, level), "every cheat applies outside 301-303");
  }

  // Every id in the table is one of the launcher's cheats (a typo would silently suppress nothing), and each level
  // is listed once.
  for (const LevelCheatsOff& r : kLevelCheatsOff) {
    Check(RuleFor(r.level) == &r, "each level listed once");
    int ids = 0;
    for (std::string_view id : r.cheats) {
      if (id.empty()) continue;
      ++ids;
      bool known = false;
      for (const kk::CheatInfo& c : kk::kCheats) known = known || id == c.id;
      Check(known, "a suppressed id is a cheat in cheats.h");
    }
    Check(ids > 0, "a rule switches at least one cheat off");
  }

  // The greyed rows' note, in all 5 languages.
  bool translated = false;
  for (const TextRow& row : kPauseText) {
    if (std::string_view(row.text[0]) != kNotInThisChapter) continue;
    translated = true;
    for (const char* t : row.text) Check(t && *t, "the note has every language");
  }
  Check(translated, "the note is in pause_text_table.inc");

  if (failures == 0) std::printf("cheat rules: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
