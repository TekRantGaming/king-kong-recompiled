// Job J8 (owner, 17 Oct): the launcher's Workshop page. A row of the owner's other projects as app-style
// tiles (name and picture; LB/RB, left/right, the mouse or its wheel move along it), then "Kong mods"
// (task 200: the mods on disk, workshop_mods.h). Selecting a tile shows the project's GitHub README inside the launcher, with [Download]
// (the latest release's page in the browser: the launcher downloads and runs nothing) and [View on GitHub].
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

namespace rex::ui {
class ImmediateDrawer;
}

namespace kk {

class Workshop {
 public:
  // `config_path`: where the Kong mods row saves its choice (kk_mods).
  Workshop(rex::ui::ImmediateDrawer* drawer, std::filesystem::path user_dir, std::filesystem::path config_path);
  ~Workshop();
  Workshop(const Workshop&) = delete;
  Workshop& operator=(const Workshop&) = delete;

  // The page between p0 and p1 (below the top bar, above the prompts). Starts the worker the first time.
  // `active`: the page has the input (no pop-up open).
  void Draw(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ui::Metrics& m, ui::Input& in, bool active);
  // Back from a project's README to the tiles (true); false on the tiles (the launcher goes Home).
  bool Back();
  std::vector<ui::Prompt> Prompts() const;
  // Developer steps (workshop:<command>): state, open:<n>, tile:<n>, download, github, link:<n>, scroll:<px>,
  // mods (focus the Kong mods row), mods:<command> (KongMods::Dev).
  void DevCommand(const std::string& command);

  // <user data>/cache/workshop.
  static std::filesystem::path CacheDir(const std::filesystem::path& user_dir);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace kk
