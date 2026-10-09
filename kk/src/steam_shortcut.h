// Add to Steam: adds the port to the player's Steam library as a non-Steam
// game, with King Kong's artwork from SteamGridDB. Steam keeps non-Steam games
// in userdata/<account>/config/shortcuts.vdf (binary VDF) and their artwork in
// that folder's grid/, named after each shortcut's app ID. Steam reads
// shortcuts.vdf only when it starts and writes its own copy when it closes, so
// the file is changed only while Steam is closed.
//
// No artwork is in the port: it is downloaded from SteamGridDB when the player
// chooses Add to Steam, and written only into their Steam artwork folder.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace kk::steam {

// Steam's folder and the accounts to add to: the one signed in last, or every
// account on this computer when Steam's login list names none of them.
struct Location {
  std::filesystem::path root;
  std::vector<std::string> accounts;
};
std::optional<Location> Locate();

// True when this copy was started from Steam (it is in the library already,
// and closing Steam would close it too).
bool StartedFromSteam();

// Whether every account's library already has this copy of the port.
bool HasShortcut(const Location& where);

bool SteamRunning();
void CloseSteam();  // asks Steam to exit (steam://exit)
void OpenSteam();   // starts Steam at its library (steam://open/games)

struct ArtPiece {
  const char* suffix;  // what Steam looks for after the app ID
  const char* url;
  std::vector<uint8_t> data;  // empty when the download failed
};
// Downloads the artwork into memory; a piece that fails is left empty.
std::vector<ArtPiece> DownloadArt();

struct Result {
  bool ok = false;
  bool added = false;  // false: it was there already and has been updated
  int art = 0;         // artwork pictures saved (of the 5)
  std::string error;
};
// Adds this copy of the port to each account's shortcuts.vdf (the old file is
// kept as shortcuts.vdf.kk-backup), saves the artwork and turns Steam Input off
// for it in localconfig.vdf (also backed up), so the game keeps seeing the
// controller. Steam must be closed.
Result AddShortcut(const Location& where, const std::vector<ArtPiece>& art);

}  // namespace kk::steam
