// Task 200 (Beta 7): the Workshop's "Kong mods" row. Every mod found in <user data>/mods and the built-ins in
// <program folder>/mods-builtin is a tile (picture or a plain panel, name, author, version, Built-in / On /
// Off badges); selecting one opens its page: description, what it changes, problems and conflicts, Enable /
// Disable and its place in the load order (Load earlier / Load later). The choice is the saved setting kk_mods
// (mods.h); the game applies it at its next start. Folders are read on a worker thread.
// Owner, 18 Oct: the row shows one kind at a time: Workshop > Mods the settings and cutscene mods (and any mod
// whose type is not known), Workshop > Texture packs the texture pack mods. Load earlier / Load later move a
// mod past the next one shown in the same row; the load order itself stays one list (kk_mods).

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include "launcher_ui.h"

namespace rex::ui {
class ImmediateDrawer;
}

namespace kk {

class KongMods {
 public:
  enum class Filter { kAll, kOthers, kTexturePacks };
  struct Counts {
    bool scanned = false;
    int mods = 0, mods_on = 0;                  // shown in Workshop > Mods
    int texture_packs = 0, texture_packs_on = 0;  // shown in Workshop > Texture packs
  };

  KongMods(rex::ui::ImmediateDrawer* drawer, std::filesystem::path user_dir, std::filesystem::path config_path);
  ~KongMods();
  KongMods(const KongMods&) = delete;
  KongMods& operator=(const KongMods&) = delete;

  // The row between p0 and p1 on the Workshop's tile page. `focused`: the row has the focus (left/right move
  // along it, A opens the mod). Returns true when the mouse took the focus to this row.
  bool DrawRow(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ui::Metrics& m, ui::Input& in, bool active, bool focused);
  // Which mods the row shows (closes an open mod page when it changes).
  void SetFilter(Filter filter);
  Counts GetCounts();
  // A mod's page is open.
  bool InDetail() const;
  void DrawDetail(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ui::Metrics& m, ui::Input& in, bool active);
  // From a mod's page back to the row (true); false when no page is open.
  bool Back();
  std::vector<ui::Prompt> Prompts(bool focused) const;
  // Developer steps (workshop:mods:<command>): state, open:<n>, openid:<id>, toggle, earlier, later, rescan, tile:<n>.
  void Dev(const std::string& command);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace kk
