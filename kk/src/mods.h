// Kong mods (Beta 7): finding the mods on disk and applying the enabled ones when the game starts.
// The format and the rules are in mods_logic.h and MODDING.md; the Workshop's "Kong mods" row is
// workshop_mods.cpp.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <rex/cvar.h>

#include "mods_logic.h"

REXCVAR_DECLARE(std::string, kk_mods);

namespace kk::mods {

// <user data>/mods (the player's mods) and <program folder>/mods-builtin (shipped with the game).
std::filesystem::path UserDir(const std::filesystem::path& user_data);
std::filesystem::path BuiltinDir();

// Reads every mod folder (built-ins first, then the player's, each sorted by folder name). Never throws:
// a mod that cannot be read comes back with its problems. Reads pack.txt and lists clips (disk work: not
// on the UI thread).
std::vector<Mod> Scan(const std::filesystem::path& user_data);

// This build's version (KK_VERSION).
const char* GameVersion();

// Compatibility fingerprint of enabled settings mods, their order/content and effective values.
// Texture packs and cutscene replacements are cosmetic and excluded. Not an authentication hash.
std::string GameplayDigest(const std::filesystem::path& user_data);

// At the game's start (before the GPU starts): applies the mods kk_mods enables. Settings mods' values go
// over the player's settings for this run only (never saved); texture packs are added to the texture pack
// folder list; cutscene folders go to the PS2 movie player. A mod that is missing or broken is skipped and
// logged. Nothing happens in a benchmark run.
void ApplyAtGameStart(const std::filesystem::path& user_data);

}  // namespace kk::mods
