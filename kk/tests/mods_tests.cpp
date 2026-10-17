// Task 200: Kong mods without a game or window: mod.toml for each type, broken files, game version ranges,
// the load order (enable, disable, move), conflicts of each type, the start plan and the texture folder list.
// (Saving with a settings mod on: hardware_recommendation_tests.cpp, with the real settings registry.)

#include <cstdio>
#include <string>

#include "mods_logic.h"

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

using namespace kk::mods;

bool Has(const Mod& m, Problem p, const std::string& detail = {}) {
  for (const auto& i : m.issues)
    if (i.problem == p && (detail.empty() || i.detail == detail)) return true;
  return false;
}

SettingCheck FakeRegistry(const std::string& name, const std::string& value) {
  if (name == "kk_hide_hud" || name == "kk_no_healing" || name == "native_cel_shading")
    return value == "true" || value == "false" ? SettingCheck::kOk : SettingCheck::kBadValue;
  if (name == "kk_fov") return value.find_first_not_of("0123456789") == std::string::npos ? SettingCheck::kOk : SettingCheck::kBadValue;
  if (name == "native_cel_outline" || name == "kk_frame_rate" || name == "kk_mods") return SettingCheck::kOk;
  return SettingCheck::kUnknown;
}

Mod Parse(const std::string& text, const std::string& folder = "m", bool builtin = false) {
  Mod m;
  m.folder = folder;
  m.id = ModId(folder, builtin);
  m.builtin = builtin;
  m.dir = std::filesystem::path("mods") / folder;
  ParseModToml(m, text, FakeRegistry);
  return m;
}

void Toml() {
  const Mod hc = Parse(R"T(
name = "Hardcore (no HUD, no healing)"
author = "TekRantGaming"
version = "1.0"
description = "No HUD. Jack's health does not come back."
type = "settings"
min_game_version = "2.0.0-beta.7"

[settings]
kk_hide_hud = true
kk_no_healing = true
)T", "hardcore", true);
  Check(hc.Usable(), "settings mod usable");
  Check(hc.name == "Hardcore (no HUD, no healing)" && hc.author == "TekRantGaming" && hc.version == "1.0", "fields");
  Check(hc.type == Type::kSettings && hc.settings.size() == 2, "two settings");
  Check(hc.settings[0].name == "kk_hide_hud" && hc.settings[0].value == "true", "bool value as text");
  Check(hc.id == "builtin:hardcore", "built-in id");

  const Mod numbers = Parse("name='N'\ntype='settings'\n[settings]\nkk_fov = 90\nnative_cel_outline = 2.5\n");
  Check(numbers.Usable() && numbers.settings.size() == 2, "numbers");
  Check(numbers.settings[0].value == "90" && numbers.settings[1].value == "2.5", "number text");

  const Mod tex = Parse("name = 'Sharp'\ntype = 'texture-pack'\nversion = 3\npicture = 'picture.png'\n");
  Check(tex.Usable() && tex.type == Type::kTexturePack, "texture pack");
  Check(tex.version == "3", "a number version reads as text");
  Check(tex.picture == std::filesystem::path("mods") / "m" / "picture.png", "picture in the folder");
  const Mod cut = Parse("name = 'Cuts'\ntype = 'cutscenes'\n");
  Check(cut.Usable() && cut.type == Type::kCutscenes, "cutscenes");

  // Broken ones: never throws, clear problems.
  Check(Has(Parse("name = 'x\ntype = 'settings'"), Problem::kBadToml), "bad TOML");
  Check(Has(Parse("type = 'settings'\n[settings]\nkk_fov = 1\n"), Problem::kNoName), "no name");
  const Mod bad_type = Parse("name = 'x'\ntype = 'code'\n");
  Check(Has(bad_type, Problem::kBadType, "code") && !bad_type.Usable(), "unknown type");
  Check(Has(Parse("name = 'x'\ntype = 'settings'\n"), Problem::kNoSettings), "settings without settings");
  const Mod skipped = Parse(
      "name='x'\ntype='settings'\n[settings]\nkk_hide_hud = true\nkk_mods = 'a'\nno_such = 1\nkk_fov = 'wide'\n"
      "native_texture_pack_folder = 'C:/x'\nkk_no_healing = [1]\n");
  Check(skipped.Usable() && skipped.settings.size() == 1, "only the good setting kept");
  Check(Has(skipped, Problem::kBlockedSetting, "kk_mods"), "kk_mods blocked");
  Check(Has(skipped, Problem::kBlockedSetting, "native_texture_pack_folder"), "paths blocked");
  Check(Has(skipped, Problem::kUnknownSetting, "no_such"), "unknown setting");
  Check(Has(skipped, Problem::kBadValue, "kk_fov"), "bad value");
  Check(Has(skipped, Problem::kBadValue, "kk_no_healing"), "array value");
  Check(Has(Parse("name='x'\ntype='texture-pack'\npicture='../../secret.png'\n"), Problem::kNoPicture), "picture outside");
  Check(Has(Parse("name='x'\ntype='texture-pack'\npicture='C:/x.png'\n"), Problem::kNoPicture) ||
            Has(Parse("name='x'\ntype='texture-pack'\npicture='/x.png'\n"), Problem::kNoPicture), "absolute picture");
  Check(!ValidFolderName("a|b") && !ValidFolderName("a:b") && !ValidFolderName(".hidden") && ValidFolderName("My Pack"),
        "folder names");
}

void Versions() {
  Check(CompareVersions("2.0.0-beta.7", "2.0.0-beta.6") > 0, "beta 7 > beta 6");
  Check(CompareVersions("2.0.0-beta.10", "2.0.0-beta.9") > 0, "beta 10 > beta 9");
  Check(CompareVersions("2.0.0", "2.0.0-beta.9") > 0, "release > beta");
  Check(CompareVersions("v2.0.0-beta7", "2.0.0-beta.7") == 0, "v and dot optional");
  Check(CompareVersions("2.0", "2.0.0") == 0, "missing parts are 0");
  Check(CompareVersions("1.9.12", "2.0.0-beta.1") < 0, "1.9.12 < 2.0.0 beta");
  Mod m = Parse("name='x'\ntype='settings'\nmin_game_version='2.0.0-beta.7'\nmax_game_version='2.0.0'\n[settings]\nkk_fov=1\n");
  Mod a = m;
  CheckGameVersion(a, "2.0.0-beta.6");
  Check(Has(a, Problem::kGameTooOld, "2.0.0-beta.7") && !a.Usable(), "too old a game");
  Mod b = m;
  CheckGameVersion(b, "2.0.0-beta.7");
  Check(b.Usable(), "in range (min)");
  Mod c = m;
  CheckGameVersion(c, "2.0.0");
  Check(c.Usable(), "in range (max)");
  Mod d = m;
  CheckGameVersion(d, "2.0.1");
  Check(Has(d, Problem::kGameTooNew, "2.0.0"), "too new a game");
}

void LoadOrder() {
  auto ids = ParseEnabled("builtin:hardcore||b|builtin:hardcore|c");
  Check(ids.size() == 3 && ids[0] == "builtin:hardcore" && ids[2] == "c", "parse, empties and repeats dropped");
  Check(JoinEnabled(ids) == "builtin:hardcore|b|c", "join");
  SetEnabled(ids, "d", true);
  Check(ids.back() == "d", "enabled goes last (wins)");
  SetEnabled(ids, "b", false);
  Check(JoinEnabled(ids) == "builtin:hardcore|c|d", "disable");
  SetEnabled(ids, "c", true);
  Check(ids.size() == 3, "enabling twice does nothing");
  Check(Move(ids, "d", -1) && JoinEnabled(ids) == "builtin:hardcore|d|c", "move earlier");
  Check(!Move(ids, "builtin:hardcore", -1), "first cannot move earlier");
  Check(Move(ids, "builtin:hardcore", 5) && ids.back() == "builtin:hardcore", "move later clamps");
  Check(!Move(ids, "zzz", 1), "unknown id");
}

void Conflicts() {
  Mod s1 = Parse("name='A'\ntype='settings'\n[settings]\nkk_fov=80\nkk_hide_hud=true\n", "a");
  Mod s2 = Parse("name='B'\ntype='settings'\n[settings]\nkk_fov=100\n", "b");
  Mod t1 = Parse("name='T1'\ntype='texture-pack'\n", "t1");
  Mod t2 = Parse("name='T2'\ntype='texture-pack'\n", "t2");
  std::string why;
  Check(ReadPackKeys("KKTP 1\n# c\n0123456789ABCDEF bf_1_2 BC3_UNORM 256 256 0123456789ABCDEF__bf_1_2.png\r\n"
                     "1111111111111111 bf_1_3 BC1_UNORM 64 64 1111111111111111__bf_1_3.dds\n",
                     t1.texture_keys, why),
        "pack keys");
  Check(t1.texture_keys.size() == 2, "two keys");
  Check(ReadPackKeys("KKTP 1\n0123456789abcdef bf_9_9 BC3_UNORM 256 256 x.png\n", t2.texture_keys, why), "pack keys 2");
  Check(!ReadPackKeys("KKTP 2\n", t2.texture_keys, why) && !why.empty(), "bad pack version");
  ReadPackKeys("KKTP 1\n0123456789abcdef bf_9_9 BC3_UNORM 256 256 x.png\n", t2.texture_keys, why);
  Mod c1 = Parse("name='C1'\ntype='cutscenes'\n", "c1");
  Mod c2 = Parse("name='C2'\ntype='cutscenes'\n", "c2");
  c1.clips = {"bk_1683.mp4", "bk_9bb9.mp4"};
  c2.clips = {"bk_9bb9.mp4"};
  const std::vector<Mod> all = {s1, s2, t1, t2, c1, c2};
  const auto enabled = EnabledInOrder(all, {"a", "t1", "c1", "b", "t2", "c2", "missing"});
  Check(enabled.size() == 6, "enabled in order, missing skipped");
  const auto conflicts = FindConflicts(enabled);
  Check(conflicts.size() == 3, "one conflict per type");
  bool settings = false, textures = false, clips = false;
  for (const auto& c : conflicts) {
    if (c.type == Type::kSettings) settings = c.earlier == "a" && c.later == "b" && c.count == 1 && c.items[0] == "kk_fov";
    if (c.type == Type::kTexturePack) textures = c.earlier == "t1" && c.later == "t2" && c.count == 1;
    if (c.type == Type::kCutscenes) clips = c.count == 1 && c.items[0] == "bk_9bb9.mp4";
  }
  Check(settings && textures && clips, "conflict details (same hash in either case)");
  Check(FindConflicts(EnabledInOrder(all, {"a", "t1"})).empty(), "different types never conflict");

  // The plan: last wins for settings; folders in load order.
  const Plan p = BuildPlan(enabled);
  Check(p.settings.size() == 2, "two distinct settings");
  Check(p.settings[0].name == "kk_fov" && p.settings[0].value == "100" && p.setting_sources[0] == "b", "later wins");
  Check(p.texture_folders.size() == 2 && p.movie_folders.size() == 2, "folders");
  Check(TextureFolderList(true, "C:/own", {"C:/a", "C:/b"}) == "C:/own|C:/a|C:/b", "own pack first");
  Check(TextureFolderList(false, "C:/own", {"C:/a"}) == "C:/a", "own pack off");
  // A broken mod is never part of the plan even when switched on.
  Mod broken = Parse("name='x'\ntype='nope'\n", "x");
  Check(EnabledInOrder({broken}, {"x"}).empty(), "broken mods are skipped");
}

void Overlay() {
  OverlayEntry e{"120", 1, "30"};
  Check(ValueToSave(&e, "30") == "120", "mod value in place: the player's own is saved");
  Check(ValueToSave(&e, "90") == "90", "changed by the player: theirs");
  Check(ValueToSave(nullptr, "45") == "45", "no mod");
}
}  // namespace

int main() {
  Toml();
  Versions();
  LoadOrder();
  Conflicts();
  Overlay();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("mods tests passed\n");
  return 0;
}
