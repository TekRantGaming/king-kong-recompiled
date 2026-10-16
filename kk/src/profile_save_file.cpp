#include "profile_save_file.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace kk::play::save_file {

uint32_t ReadWord(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return std::byteswap(v);
}
void WriteWord(uint8_t* p, uint32_t v) {
  v = std::byteswap(v);
  std::memcpy(p, &v, 4);
}

std::vector<uint8_t> ReadSave(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in || in.tellg() != std::streampos(kSaveSize)) return {};
  std::vector<uint8_t> bytes(kSaveSize);
  in.seekg(0);
  if (!in.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) || ReadWord(bytes.data() + 0x28) == 0 ||
      ReadWord(bytes.data() + 0xA68) != 4) return {};
  return bytes;
}

bool OptionsValid(const uint8_t* bytes, const uint32_t* fields) {
  for (int i = 0; i < 4; ++i) {
    const float v = std::bit_cast<float>(ReadWord(bytes + fields[i]));
    if (!std::isfinite(v) || v < 0 || v > 1.001f) return false;
  }
  for (int i = 4; i < kOptionCount; ++i) if (ReadWord(bytes + fields[i]) > (i == 5 ? 2u : 1u)) return false;
  return true;
}

namespace {
// Keeps the newest kBackupsPerSlot backups of a slot; older ones are removed.
constexpr size_t kBackupsPerSlot = 3;
void PruneBackups(const std::filesystem::path& backup_dir, int slot) {
  const std::string prefix = "slot" + std::to_string(slot + 1) + "-";
  std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> backups;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(backup_dir, ec), end; !ec && it != end; it.increment(ec)) {
    const auto name = it->path().filename().string();
    if (!name.starts_with(prefix) || it->path().extension() != ".dat") continue;
    std::error_code time_ec;
    backups.emplace_back(std::filesystem::last_write_time(it->path(), time_ec), it->path());
  }
  if (backups.size() <= kBackupsPerSlot) return;
  std::sort(backups.begin(), backups.end());
  for (size_t i = 0; i + kBackupsPerSlot < backups.size(); ++i) {
    std::error_code remove_ec;
    std::filesystem::remove(backups[i].second, remove_ec);
  }
}
}  // namespace

// Keep the original file as a backup and replace only after the entire new file is
// written. No header, progress, timestamp or unrelated option bytes are changed.
bool WriteOptions(const std::filesystem::path& path, const std::filesystem::path& backup_dir, int slot,
                  const Values& values, const Changed& changed) {
  auto bytes = ReadSave(path);
  if (bytes.empty() || !OptionsValid(bytes.data(), kFileFields)) return false;
  bool different = false;
  for (int i = 0; i < kOptionCount; ++i) {
    if (!changed[size_t(i)]) continue;
    const float value = values[size_t(i)];
    const uint32_t bits = i >= 4 ? uint32_t(value != 0) : std::bit_cast<uint32_t>(value / 100.0f);
    different |= ReadWord(bytes.data() + kFileFields[i]) != bits;
    WriteWord(bytes.data() + kFileFields[i], bits);
    if (i == 8) {
      different |= ReadWord(bytes.data() + kInventoryCompanion) != bits;
      WriteWord(bytes.data() + kInventoryCompanion, bits);
    }
  }
  if (!different) return true;
  static uint64_t serial = 0;
  const auto stamp = std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial);
  const auto pending = path.parent_path() / ("KKProfile.audio-pending-" + stamp);
  const auto backup = backup_dir / ("slot" + std::to_string(slot + 1) + "-" + stamp + ".dat");
  std::error_code ec;
  std::filesystem::create_directories(backup_dir, ec);
  if (ec) return false;
  {
    std::ofstream out(pending, std::ios::binary);
    if (!out || !out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size())) return false;
    out.flush();
    if (!out) return false;
  }
  auto discard_pending = [&pending] {
    std::error_code ignored;
    std::filesystem::remove(pending, ignored);
  };
  if (ReadSave(pending) != bytes) { discard_pending(); return false; }
  // The save stays in place throughout: copy it to the backup, then replace it in one step.
  std::filesystem::copy_file(path, backup, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) { discard_pending(); return false; }
#if defined(_WIN32)
  if (!MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    discard_pending();
    return false;
  }
#else
  std::filesystem::rename(pending, path, ec);  // atomic replacement on POSIX
  if (ec) {
    discard_pending();
    return false;
  }
#endif
  PruneBackups(backup_dir, slot);
  return true;
}

AllSavesResult WriteOptionsToSaves(const std::function<std::filesystem::path(int)>& slot_file, int slots,
                                   const std::filesystem::path& backup_dir, const Values& values,
                                   const Changed& changed, int skip_slot) {
  AllSavesResult result;
  for (int slot = 0; slot < slots; ++slot) {
    if (slot == skip_slot) continue;
    const auto path = slot_file(slot);
    if (path.empty() || ReadSave(path).empty()) continue;  // no save in this slot
    if (WriteOptions(path, backup_dir, slot, values, changed)) ++result.written;
    else ++result.failed;
  }
  return result;
}

}  // namespace kk::play::save_file
