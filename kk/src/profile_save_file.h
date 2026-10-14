// The audio and subtitle options inside the game's save files (KKProfile.dat, one per slot), read and
// written without the game: profile_options.cpp uses it for the launcher's and the pause menu's Audio
// and Subtitles rows, which apply to every save. Kept apart from the game hooks so it can be tested on
// its own (tests/launcher_logic_tests.cpp).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

namespace kk::play::save_file {

constexpr size_t kSaveSize = 0x8040;
// Master, voices, music, effects (big-endian floats, 0 to 1), then subtitles (a word, 0 or 1).
constexpr uint32_t kFileFields[] = {0xA78, 0xA70, 0xA6C, 0xA74, 0x1154};
constexpr int kOptionCount = 5;
using Values = std::array<float, kOptionCount>;  // volumes in percent, subtitles 0 or 1
using Changed = std::array<bool, kOptionCount>;

uint32_t ReadWord(const uint8_t* p);
void WriteWord(uint8_t* p, uint32_t v);

// A slot's save, or nothing when there is none: no file, another size, or the empty header the game
// writes for an unused slot.
std::vector<uint8_t> ReadSave(const std::filesystem::path& path);
// The options in a save are in range (a save this code can safely change).
bool OptionsValid(const uint8_t* bytes, const uint32_t* fields);

// Writes the changed options into one save: the file is written whole under a temporary name, checked,
// the save copied to `backup_dir` (the newest three per slot are kept), then replaced in one step. No
// other byte changes. False when it could not be done; the save is then unchanged.
bool WriteOptions(const std::filesystem::path& save, const std::filesystem::path& backup_dir, int slot,
                  const Values& values, const Changed& changed);

// Every slot that holds a save, except `skip_slot` (the game's own, which the game thread writes).
struct AllSavesResult {
  int written = 0;  // saves now holding the values
  int failed = 0;   // saves that could not be written (left unchanged)
};
AllSavesResult WriteOptionsToSaves(const std::function<std::filesystem::path(int)>& slot_file, int slots,
                                   const std::filesystem::path& backup_dir, const Values& values,
                                   const Changed& changed, int skip_slot);

}  // namespace kk::play::save_file
