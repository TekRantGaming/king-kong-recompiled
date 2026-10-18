// Task 209 (owner, 18 Oct): the Workshop's Texture packs row and a texture pack's page. Every texture pack the
// player has or can get is a card: the original textures, the HD texture pack the setup offers (a content
// download, content.h), Kong UHD Textures and Kong Comic Book (Workshop downloads that install as texture pack mods,
// content::kModPacks and texture_packs_downloads.h), every other
// texture pack mod (imported with Import texture pack or put in the mods folder by hand) and the player's own
// folder. Only one is on at a time (texture_packs_logic.h): switching one on switches the one before off.
// The state lives in the settings (native_texture_pack, native_texture_pack_folder, kk_mods,
// kk_texture_pack_own_folder), so Settings > Graphics and this row always agree.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "launcher_ui.h"

namespace rex::ui {
class ImmediateTexture;
}

namespace kk {

class KongMods;
struct WorkshopHost;

class TexturePackShelf {
 public:
  // A card of the row (workshop.cpp draws the row the same way for its three rows).
  struct Card {
    std::string key;          // "original", "hd", "uhd", "comic", "mod:<id>", "folder", "import" (the action card)
    std::string name, sub;    // the band: name, then author and version (or what the card does)
    std::string hint;         // one line about it, shown beside the row's caption while it has the focus
    rex::ui::ImmediateTexture* picture = nullptr;
    float aspect = 16.0f / 9.0f;
    std::string state;        // the badge: Enabled, Disabled, Not installed, Downloading 45%, ...
    ImU32 state_bg = 0, state_fg = 0;
    bool action = false;      // a button card (Import texture pack)
    float progress = -1;      // a download or install running: its bar (0..1), -1 none
  };

  TexturePackShelf(std::filesystem::path user_dir, KongMods* mods);
  ~TexturePackShelf();
  void SetHost(const WorkshopHost* host);

  std::vector<Card> Cards();
  // A card chosen on the row: its page, or the action.
  void Activate(const std::string& key);
  bool InDetail() const;
  bool Back();
  std::string DetailTitle() const;
  void DrawDetail(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ui::Metrics& m, ui::Input& in, bool active);

  // Settings > Graphics: (key, name) of the packs that can be switched on now (the original textures first),
  // the key of the one on, and switching one on.
  std::vector<std::pair<std::string, std::string>> Choices();
  std::string Active();
  void Enable(const std::string& key);

  void OnModInstalled(const std::string& id);
  void OnModRemoved(const std::string& id);
  void OnFolderOn();
  void Tick();
  void Dev(const std::string& command);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace kk
