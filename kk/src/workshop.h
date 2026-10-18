// Job J8 (owner, 17 Oct): the launcher's Workshop page. Owner, 18 Oct: the Workshop opens a choice of three
// (the hub, drawn by the launcher as a menu like Play's): Mods, Texture packs, Other TRG projects and games.
//  - Projects: a row of the owner's other projects as app-style tiles (name and picture; LB/RB, left/right,
//    the mouse or its wheel move along it). Selecting a tile shows the project's GitHub README inside the
//    launcher, with [Download] (the latest release's page in the browser: the launcher downloads and runs
//    nothing) and [View on GitHub].
//  - Mods: the "Kong mods" row (task 200: the mods on disk, workshop_mods.h) without the texture pack mods,
//    then Modding guide (MODDING.md beside the game, shown like a README) and Open mods folder.
//  - Texture packs: the launcher draws the HD texture pack rows; this class draws the texture pack mods row
//    under them (DrawTextureModsRow) and a mod's page.
//
// Everything about the projects (READMEs, repository facts, pictures) is fetched at run time on the
// page's own worker thread, cached in <user data>/cache/workshop and asked for again at most once a day
// (ETag / Last-Modified): nothing is shipped or committed. Offline without a cache, a tile is its name on
// a plain panel. The UI thread never waits for the network or the disk.

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include "launcher_ui.h"
#include "workshop_mods.h"

namespace rex::ui {
class ImmediateDrawer;
}

namespace kk {

class Workshop {
 public:
  enum class View { kHub, kMods, kTexturePacks, kProjects };

  // `config_path`: where the Kong mods row saves its choice (kk_mods).
  Workshop(rex::ui::ImmediateDrawer* drawer, std::filesystem::path user_dir, std::filesystem::path config_path);
  ~Workshop();
  Workshop(const Workshop&) = delete;
  Workshop& operator=(const Workshop&) = delete;

  // Which page shows (the launcher sets kHub when the Workshop opens; developer steps may change it).
  void SetView(View view);
  View view() const;
  // A page inside the view is open: a project's README, a mod's page or the modding guide.
  bool InDetail() const;
  // The view's page between p0 and p1 (below the top bar, above the prompts); in Texture packs only a mod's
  // page (InDetail). Projects starts the network worker the first time. `active`: the page has the input.
  void Draw(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ui::Metrics& m, ui::Input& in, bool active);
  // Texture packs: the texture pack mods row. Returns true when the mouse took the focus to it.
  bool DrawTextureModsRow(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ui::Metrics& m, ui::Input& in, bool active,
                          bool focused);
  KongMods::Counts ModCounts();
  // Back from a page inside the view (true); false otherwise (the launcher goes back to the hub).
  bool Back();
  // `row_focused`: Texture packs, the mods row has the focus.
  std::vector<ui::Prompt> Prompts(bool row_focused = false) const;
  // Developer steps (workshop:<command>): state, open:<n>, tile:<n>, download, github, link:<n>, scroll:<px>
  // (Projects); mods (Mods), mods:<command> (KongMods::Dev), guide, folder; view:<hub|mods|textures|projects>.
  void DevCommand(const std::string& command);

  // <user data>/cache/workshop.
  static std::filesystem::path CacheDir(const std::filesystem::path& user_dir);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace kk
