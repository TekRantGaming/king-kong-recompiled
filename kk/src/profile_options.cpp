#include "play_session.h"
#include "play_internal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

#include <rex/hook.h>
#include <rex/cvar.h>
#include <rex/logging.h>

REX_EXTERN(__imp__sub_82413298);
#if defined(KK_DEV_TOOLS)
REX_EXTERN(__imp__sub_82413190);
REX_EXTERN(__imp__sub_827291A0);
namespace kk { double DevSecondsSinceMenu(); }
#endif

namespace kk::play {
namespace {
constexpr uint32_t kFields[] = {18712, 18704, 18700, 18708, 19856};
// Filled from the universe's saved-variable descriptors, verified by KK_DEV_AUDIO_TRACE.
constexpr uint32_t kFileFields[] = {0xA78, 0xA70, 0xA6C, 0xA74, 0x1154};
constexpr size_t kSaveSize = 0x8040;
std::mutex g_options_mutex;
ProfileOptions g_live_options;
std::string g_error;
int g_live_slot = -1;
std::array<float, 5> g_pending{};
std::array<bool, 5> g_dirty{};

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

ProfileOptions Decode(const uint8_t* bytes, const uint32_t* fields, bool percent) {
  ProfileOptions out;
  const auto language = rex::cvar::GetFlagByName("user_language");
  // The game's language mapping: German, Spanish and Italian use subtitled speech.
  out.subtitle_default = language == "3" || language == "5" || language == "6";
  float* volumes[] = {&out.master, &out.voices, &out.music, &out.effects};
  for (int i = 0; i < 4; ++i) {
    const float v = std::bit_cast<float>(ReadWord(bytes + fields[i]));
    if (!std::isfinite(v) || v < 0 || v > 1.001f) return {};
    *volumes[i] = v * (percent ? 100 : 1);
  }
  const uint32_t subtitles = ReadWord(bytes + fields[4]);
  if (subtitles > 1) return {};
  out.subtitles = subtitles != 0;
  out.available = true;
  return out;
}

// Keep the original file as a backup and replace only after the entire new file is
// written. No header, progress, timestamp or unrelated option bytes are changed.
bool WriteOptions(const std::filesystem::path& root, int slot, const std::array<float, 5>& values,
                  const std::array<bool, 5>& dirty) {
  const auto path = ProfileFilePath(root, slot);
  auto bytes = ReadSave(path);
  if (bytes.empty() || !Decode(bytes.data(), kFileFields, true).available) return false;
  bool changed = false;
  for (int i = 0; i < 5; ++i) {
    if (!dirty[i]) continue;
    const uint32_t bits = i == 4 ? uint32_t(values[i] != 0) : std::bit_cast<uint32_t>(values[i] / 100.0f);
    changed |= ReadWord(bytes.data() + kFileFields[i]) != bits;
    WriteWord(bytes.data() + kFileFields[i], bits);
  }
  if (!changed) return true;
  static uint64_t serial = 0;
  const auto stamp = std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial);
  const auto backup_dir = root / "audio-option-backups";
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
  if (ReadSave(pending) != bytes) return false;
  std::filesystem::rename(path, backup, ec);
  if (ec) return false;
  std::filesystem::rename(pending, path, ec);
  if (ec) {
    std::error_code rollback;
    std::filesystem::rename(backup, path, rollback);
    REXLOG_ERROR("KK audio: replacing the profile failed: {}; rollback {}", ec.message(), rollback.message());
    return false;
  }
  return true;
}

// Guard the hard-coded disk layout against a different game image or script data.
bool VerifyLayout(const uint8_t* base) {
  static bool verified = false;
  if (verified) return true;
  const uint32_t script = ReadWord(base + 0x828E838C);
  const uint32_t object = script ? ReadWord(base + script) : 0;
  const uint32_t desc = object ? ReadWord(base + object) : 0;
  if (!desc) return false;
  const uint32_t table = ReadWord(base + desc + 8), count = ReadWord(base + desc + 12);
  if (!table || !count || count > 16384) return false;
  std::array<bool, 5> found{};
  uint32_t offset = 0x100;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t d = table + i * 16, variable = ReadWord(base + d), shape = ReadWord(base + d + 8);
    const uint32_t type = uint32_t(base[d + 12]) << 8 | base[d + 13];
    const uint32_t flags = uint32_t(base[d + 14]) << 8 | base[d + 15];
    if (!(flags & 0x10)) continue;
    if (type > 255) return false;
    const uint32_t size = uint32_t(base[0x828CEFF0 + type * 8 + 2]) << 8 | base[0x828CEFF0 + type * 8 + 3];
    const uint32_t bytes = (shape >> 30) * 4 + (shape & 0x3FFFFFFF) * size;
    for (int j = 0; j < 5; ++j)
      if (kFields[j] >= variable && kFields[j] + 4 <= variable + bytes && (flags & 0x40) &&
          offset + kFields[j] - variable == kFileFields[j]) found[j] = true;
    offset += bytes;
    if (offset > kSaveSize) return false;
  }
  verified = std::all_of(found.begin(), found.end(), [](bool b) { return b; });
  if (verified) REXLOG_INFO("KK audio: game's saved-option layout verified (four volume words and subtitles)");
  return verified;
}
}  // namespace

ProfileOptions ReadProfileOptions(const std::filesystem::path& root, int slot) {
  std::lock_guard lock(g_options_mutex);
  if (slot < 0) return g_live_options;
  const auto data = ReadSave(ProfileFilePath(root, slot));
  if (data.empty()) return {};
  return Decode(data.data(), kFileFields, true);
}

bool SetProfileOption(const std::filesystem::path& root, int slot, ProfileOption option, float value) {
  const int i = int(option);
  if (i < 0 || i >= 5 || !std::isfinite(value)) return false;
  value = i == 4 ? float(value != 0) : std::clamp(value, 0.0f, 100.0f);
  std::lock_guard lock(g_options_mutex);
  if (slot >= 0) {
    std::array<float, 5> values{};
    std::array<bool, 5> dirty{};
    values[i] = value; dirty[i] = true;
    return WriteOptions(root, slot, values, dirty);
  }
  if (!g_live_options.available || g_live_slot < 0) return false;
  g_pending[i] = value; g_dirty[i] = true;
  return true;
}

void ServiceProfileOptions(PPCContext& ctx, uint8_t* base) {
  if (!VerifyLayout(base)) return;
  const uint32_t g = ReadWord(base + 0x82CC8D24);
  if (!g) return;
  const int slot = int32_t(ReadWord(base + g + 0x4E10));
  std::lock_guard lock(g_options_mutex);
  if (slot != g_live_slot) g_dirty.fill(false);
  g_live_slot = slot;
  if (slot >= 0 && slot < kSlotCount && !ProfileSaveBusy(base) &&
      std::any_of(g_dirty.begin(), g_dirty.end(), [](bool b) { return b; })) {
    if (WriteOptions(UserDir(), slot, g_pending, g_dirty)) {
      g_error.clear();
      for (int i = 0; i < 5; ++i) {
        if (!g_dirty[i]) continue;
        const uint32_t bits = i == 4 ? uint32_t(g_pending[i] != 0) : std::bit_cast<uint32_t>(g_pending[i] / 100.0f);
        WriteWord(base + g + kFields[i], bits);
        // Chapter-select keeps a restart copy of the serialized variables.
        if (ReadWord(base + g + 0x5668)) WriteWord(base + 0x82C6BDE0 + kFileFields[i] - 0x100, bits);
        REXLOG_INFO("KK audio: applied option {} value {} to slot {} and its save", i, g_pending[i], slot + 1);
      }
      WriteWord(base + g + 18692, ReadWord(base + g + 18692) | 1);
      const PPCContext saved = ctx;
      __imp__sub_82413298(ctx, base);  // the old Options page's Accept: snapshot saved options
      ctx = saved;
    } else {
      g_error = "The setting could not be saved. Your save was left unchanged.";
      REXLOG_ERROR("KK audio: could not save the options; the game profile was left unchanged");
    }
    g_dirty.fill(false);
  }
  g_live_options = Decode(base + g, kFields, true);
  g_live_options.subtitle_default = ReadWord(base + g + 20456) != 0;
  g_live_options.error = g_error;
  g_live_options.available = g_live_options.available && slot >= 0 && slot < kSlotCount;
#if defined(KK_DEV_TOOLS)
  // A real game save job after UI tests, without a story run's long opening movie.
  // Retain the existing header and continue point. Only enabled in private test runs.
  if (const char* at = std::getenv("KK_DEV_AUDIO_SAVE_AT")) {
    static bool sent = false;
    if (!sent && kk::DevSecondsSinceMenu() >= std::atof(at) && g_live_options.available && !ProfileSaveBusy(base) &&
        ReadWord(base + 0x82C6399C + uint32_t(slot) * 0x100 + 0x28) != 0) {
      sent = true;
      const PPCContext saved = ctx;
      __imp__sub_82413190(ctx, base);
      ctx = saved;
      ctx.r3.u64 = 0x20000060;
      ctx.r4.u64 = uint32_t(slot);
      __imp__sub_827291A0(ctx, base);
      ctx = saved;
      REXLOG_INFO("KK audio test: queued the game's save job for slot {}", slot + 1);
    }
  }
#endif
}
}  // namespace kk::play
