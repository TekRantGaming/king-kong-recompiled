// Task 209: only one texture pack on at a time (texture_packs_logic.h): the HD texture pack and the player's own
// folder (settings native_texture_pack + native_texture_pack_folder) and the texture pack mods (kk_mods), while
// mods of the other types stay many at once in their load order.
#include "texture_packs_logic.h"

#include <iostream>

using namespace kk::texpacks;

static int failures = 0;
#define CHECK(x)                                                              \
  do {                                                                        \
    if (!(x)) {                                                               \
      ++failures;                                                             \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #x "\n"; \
    }                                                                         \
  } while (0)

int main() {
  // Texture pack mods: Kong UHD Textures, an imported pack, a pack the player put in the mods folder.
  const IsTexturePackMod is_tex = [](const std::string& id) {
    return id == "Kong UHD Textures" || id == "my-pack" || id == "hand-made";
  };
  const std::string hd = "C:/user/content/hd-textures/kk-4x-usv2";
  Choice c;
  c.mods = {"builtin:cel-shading", "Fast Kong"};  // two settings mods, both on
  CHECK(CountOn(c, is_tex) == 0 && ActiveId(c, is_tex).empty());

  // The HD texture pack on.
  c = EnableFolder(c, hd, is_tex);
  CHECK(c.folder_on && c.folder == hd && CountOn(c, is_tex) == 1 && ActiveId(c, is_tex) == "folder:" + hd);
  CHECK((c.mods == std::vector<std::string>{"builtin:cel-shading", "Fast Kong"}));  // other mods untouched

  // UHD on: the HD pack goes off (its folder is kept for later), the settings mods stay.
  c = EnableMod(c, "Kong UHD Textures", is_tex);
  CHECK(!c.folder_on && c.folder == hd && CountOn(c, is_tex) == 1 && ActiveId(c, is_tex) == "mod:Kong UHD Textures");
  CHECK((c.mods == std::vector<std::string>{"builtin:cel-shading", "Fast Kong", "Kong UHD Textures"}));

  // Another texture pack mod (a new import): UHD goes off.
  c = EnableMod(c, "my-pack", is_tex);
  CHECK(CountOn(c, is_tex) == 1 && ActiveId(c, is_tex) == "mod:my-pack");
  CHECK((c.mods == std::vector<std::string>{"builtin:cel-shading", "Fast Kong", "my-pack"}));

  // Back to the HD pack: the mod goes off.
  c = EnableFolder(c, hd, is_tex);
  CHECK(CountOn(c, is_tex) == 1 && ActiveId(c, is_tex) == "folder:" + hd);
  CHECK((c.mods == std::vector<std::string>{"builtin:cel-shading", "Fast Kong"}));

  // The player's own folder replaces the HD pack (same two settings).
  c = EnableFolder(c, "D:/my textures", is_tex);
  CHECK(CountOn(c, is_tex) == 1 && c.folder == "D:/my textures");

  // The game's own textures: nothing on; enabling the same mod twice keeps it once.
  c = DisableAll(c, is_tex);
  CHECK(CountOn(c, is_tex) == 0 && ActiveId(c, is_tex).empty() && c.folder == "D:/my textures");
  c = EnableMod(EnableMod(c, "hand-made", is_tex), "hand-made", is_tex);
  CHECK((c.mods == std::vector<std::string>{"builtin:cel-shading", "Fast Kong", "hand-made"}));

  // Old settings with several on: one kept, always the same one (the texture pack mod last in the load order,
  // whose textures won before); settings mods untouched; already-normal settings are left exactly as they are.
  Choice old;
  old.folder_on = true;
  old.folder = hd;
  old.mods = {"my-pack", "builtin:cel-shading", "Kong UHD Textures", "Fast Kong"};
  const Choice n = Normalize(old, is_tex);
  CHECK(CountOn(n, is_tex) == 1 && !n.folder_on && n.folder == hd);
  CHECK((n.mods == std::vector<std::string>{"builtin:cel-shading", "Kong UHD Textures", "Fast Kong"}));
  CHECK(Normalize(n, is_tex) == n && Normalize(old, is_tex) == n);  // stable and deterministic
  CHECK(ActiveId(old, is_tex) == "mod:Kong UHD Textures");
  Choice only_folder;
  only_folder.folder_on = true;
  only_folder.folder = hd;
  only_folder.mods = {"Fast Kong"};
  CHECK(Normalize(only_folder, is_tex) == only_folder);
  Choice two_mods;
  two_mods.mods = {"hand-made", "my-pack"};
  CHECK((Normalize(two_mods, is_tex).mods == std::vector<std::string>{"my-pack"}));
  // A folder switch that is on without a folder counts as nothing on.
  Choice empty_folder;
  empty_folder.folder_on = true;
  empty_folder.mods = {"my-pack"};
  CHECK(CountOn(empty_folder, is_tex) == 1 && Normalize(empty_folder, is_tex) == empty_folder);

  if (failures) std::cerr << failures << " check(s) failed\n";
  else std::cout << "texture pack rules: all checks passed\n";
  return failures ? 1 : 0;
}
