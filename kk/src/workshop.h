// Job J8 (owner, 17 Oct): the launcher's Workshop page. Owner, 18 Oct (task 209, replacing the menu of three):
// ONE page with three rows of cards, top to bottom:
//  - Texture packs (workshop_texpacks.h): the original textures, the HD texture pack the setup offers, the Kong
//    UHD Textures download, every texture pack mod (imported ones too) and the player's own folder. Only one is
//    on at a time. Import texture pack is the row's last card.
//  - Mods: the settings and cutscene mods (workshop_mods.h: their pages, Enable / Disable, load order; many at
//    once), then Import mod, Modding guide, Open mods folder and Build your own mods and texture packs.
//  - Other TRG projects and games: the owner's other projects as app-style tiles; a tile opens the project's
//    GitHub README inside the launcher, with [Download] (the latest release's page in the browser: the launcher
//    downloads and runs nothing) and [View on GitHub].
// Up and Down move between the rows, Left and Right (LB and RB) along one; each row keeps its own focus. A card
// opens its page (a texture pack's, a mod's, a project's README); Back comes back to the same card.
//
// Everything about the projects (READMEs, repository facts, pictures) is fetched at run time on the
// page's own worker thread, cached in <user data>/cache/workshop and asked for again at most once a day
// (ETag / Last-Modified): nothing is shipped or committed. Offline without a cache, a tile is its name on
// a plain panel. The UI thread never waits for the network or the disk.

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include "content.h"
#include "launcher_ui.h"
#include "workshop_mods.h"

namespace rex::ui {
class ImmediateDrawer;
class ImmediateTexture;
}

namespace kk {

// What the launcher does for the Workshop: its pop-ups, file dialogs and downloads (launcher.cpp).
struct WorkshopHost {
  std::function<void(content::Kind)> download;       // "This can take a while", then the download
  std::function<void(content::Kind)> progress;       // the progress pop-up of a download that runs
  std::function<void(content::Kind)> remove;         // the HD texture pack's Remove (asks first)
  std::function<void(content::ImportTarget)> import_zip;  // the file dialog, then the import with its progress
  std::function<void()> choose_folder;               // the player's own texture pack folder
  // A pop-up: title and text (English, translated when shown), buttons, the focused one, what a choice does.
  std::function<void(const std::string&, const std::string&, std::vector<std::string>, int, std::function<void(int)>)> ask;
  std::function<void(const std::string&)> status;    // the launcher's status line (translated by the caller)
  std::function<void(const std::string&)> open_url;  // in the browser
  // The launcher's stills from the game (its backdrops): the pictures of the cards that have none of their own.
  std::function<rex::ui::ImmediateTexture*(int index, float* aspect)> still;
  // Task 221: a card picture made at run time from a still of the player's own game (workshop_art.h) for "hd"
  // (the HD texture pack) and built-in mods ("builtin:cel-shading"); nullptr while there is no still.
  std::function<rex::ui::ImmediateTexture*(const std::string& key, float* aspect)> art;
  // Task 221: a texture from RGBA8 pixels (a download's own picture.png, read before the pack is downloaded).
  std::function<rex::ui::ImmediateTexture*(int width, int height, const std::vector<uint8_t>& rgba)> texture;
  // The texture pack rule changed the settings: save them.
  std::function<void()> save;
};

class Workshop {
 public:
  // `config_path`: where the mods' choice (kk_mods) and the texture pack choice are saved.
  Workshop(rex::ui::ImmediateDrawer* drawer, std::filesystem::path user_dir, std::filesystem::path config_path);
  ~Workshop();
  Workshop(const Workshop&) = delete;
  Workshop& operator=(const Workshop&) = delete;

  void SetHost(WorkshopHost host);
  // The Workshop page was opened (from Home or elsewhere): the projects row gets a fresh random order (task 221).
  void OnOpen();
  // A page inside the Workshop is open: a texture pack's, a mod's, a project's README or the modding guide.
  bool InDetail() const;
  // The page between p0 and p1 (below the top bar, above the prompts). Starts the network worker the first time.
  // `active`: the page has the input.
  void Draw(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ui::Metrics& m, ui::Input& in, bool active);
  // Back from a page inside (true); false on the rows (the launcher goes back to Home).
  bool Back();
  std::vector<ui::Prompt> Prompts() const;
  // The title of the open page (for the top bar), "" on the rows.
  std::string DetailTitle() const;
  // Developer steps (workshop:<command>): state, row:<0-2>, card:<n>, open (the focused card), open:<n> (a project),
  // tile:<n> (a project), download, github, link:<n>, links, scroll:<px> (a README); mods:<command>
  // (KongMods::Dev), guide, folder; tex:<command> (TexturePackShelf::Dev).
  void DevCommand(const std::string& command);

  KongMods& Mods();
  // The texture pack choices of Settings > Graphics: (key, name) of every pack that can be switched on now, the
  // original textures first; and switching one on (the same rule as the Workshop's row).
  std::vector<std::pair<std::string, std::string>> TexturePackChoices();
  std::string ActiveTexturePack();
  void EnableTexturePack(const std::string& key);
  // Task 365: a texture pack card's picture (TexturePackShelf::Picture), for the first time setup's Workshop page.
  rex::ui::ImmediateTexture* TexturePackPicture(const std::string& key, float* aspect);
  // The launcher heard from the content service (TickContent): a texture pack mod was installed (it becomes the
  // one on), a mod was installed or removed, or the HD texture pack was turned on.
  void OnTexturePackModInstalled(const std::string& id);
  void OnModsChanged(const std::string& removed_id);
  void OnFolderPackOn();
  // Once the first mods scan is in: settings with several texture packs on keep one (texture_packs_logic.h).
  void Tick();

  // <user data>/cache/workshop.
  static std::filesystem::path CacheDir(const std::filesystem::path& user_dir);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace kk
