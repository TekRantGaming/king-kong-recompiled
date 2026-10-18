// Task 209 (owner, 18 Oct): only ONE texture pack is on at a time. The packs a player can have:
//  - folder packs, turned on by the settings native_texture_pack (on/off) + native_texture_pack_folder (which):
//    the HD texture pack the setup offers (content/hd-textures/...) and the player's own folder;
//  - texture pack mods (mods_logic.h, type texture-pack) in the mods folder, turned on by being in kk_mods:
//    Kong UHD Textures (a Workshop download), every pack imported with Import texture pack, and any the player
//    put there by hand.
// Mods of the other types stay many at once, with their load order. These rules are pure (tests:
// tests/texture_packs_tests.cpp); texture_packs.cpp applies them to the settings, and every place that turns a
// pack on goes through them: the Workshop's texture pack row and its pages, Settings > Graphics > Texture pack,
// an import or download that finishes, and the start of the launcher and of the game (Normalize).
#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace kk::texpacks {

struct Choice {
  bool folder_on = false;      // native_texture_pack
  std::string folder;          // native_texture_pack_folder
  std::vector<std::string> mods;  // kk_mods, in load order (mods of every type)
  bool operator==(const Choice&) const = default;
};

// Whether a kk_mods entry is a texture pack mod (the mods scan knows; unknown ids count as not).
using IsTexturePackMod = std::function<bool(const std::string& id)>;

inline int CountOn(const Choice& c, const IsTexturePackMod& is_tex) {
  int n = c.folder_on && !c.folder.empty() ? 1 : 0;
  for (const auto& id : c.mods) n += is_tex(id) ? 1 : 0;
  return n;
}

inline void DropTextureMods(Choice& c, const IsTexturePackMod& is_tex, const std::string& keep = {}) {
  c.mods.erase(std::remove_if(c.mods.begin(), c.mods.end(),
                              [&](const std::string& id) { return id != keep && is_tex(id); }),
               c.mods.end());
}

// A folder pack on (the HD texture pack, the player's own folder): every texture pack mod off.
inline Choice EnableFolder(Choice c, const std::string& folder, const IsTexturePackMod& is_tex) {
  c.folder_on = true;
  c.folder = folder;
  DropTextureMods(c, is_tex);
  return c;
}

// A texture pack mod on: the folder pack off (its folder is kept, so it can come back) and every other texture
// pack mod off. The mod goes to the end of the load order, as mods_logic.h SetEnabled does.
inline Choice EnableMod(Choice c, const std::string& id, const IsTexturePackMod& is_tex) {
  c.folder_on = false;
  DropTextureMods(c, is_tex);
  c.mods.erase(std::remove(c.mods.begin(), c.mods.end(), id), c.mods.end());
  c.mods.push_back(id);
  return c;
}

// The game's own textures: no pack on.
inline Choice DisableAll(Choice c, const IsTexturePackMod& is_tex) {
  c.folder_on = false;
  DropTextureMods(c, is_tex);
  return c;
}

// Settings from an older build (or a pause menu switch, or a mod switched on by hand in the settings file) may
// have several on. One is kept, always the same way: the texture pack mod latest in the load order, which is the
// one whose textures won when several were loaded (the loader lets a later pack win); with no texture pack mod
// on, the folder pack stays as it is.
inline Choice Normalize(Choice c, const IsTexturePackMod& is_tex) {
  if (CountOn(c, is_tex) <= 1) return c;
  std::string last;
  for (const auto& id : c.mods)
    if (is_tex(id)) last = id;
  if (last.empty()) return c;  // only the folder pack: already one
  c.folder_on = false;
  DropTextureMods(c, is_tex, last);
  return c;
}

// Which pack is on: "mod:<id>", "folder:<folder>", or "" (the game's own textures). After Normalize there is
// at most one; before it, the one Normalize would keep.
inline std::string ActiveId(const Choice& c, const IsTexturePackMod& is_tex) {
  const Choice n = Normalize(c, is_tex);
  for (const auto& id : n.mods)
    if (is_tex(id)) return "mod:" + id;
  if (n.folder_on && !n.folder.empty()) return "folder:" + n.folder;
  return {};
}

}  // namespace kk::texpacks
