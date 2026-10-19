#include "mods.h"
#include "texture_packs_logic.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <system_error>

#include <rex/filesystem.h>
#include <rex/logging.h>

#include "ps2_movies.h"
#include "settings.h"

REXCVAR_DEFINE_STRING(kk_mods, "", "KK",
                      "Kong mods switched on, in load order (Workshop > Kong mods): ids separated by |");
// Task 209: the player's own texture pack folder, kept apart from native_texture_pack_folder (which holds the
// folder of the pack that is on: the HD texture pack or this one), so switching between them loses neither.
REXCVAR_DEFINE_STRING(kk_texture_pack_own_folder, "", "KK",
                      "Your own texture pack folder (Workshop > Texture packs, Settings > Graphics)");

#ifndef KK_VERSION
#define KK_VERSION ""
#endif

namespace kk::mods {
namespace fs = std::filesystem;

namespace {

bool ReadSmall(const fs::path& path, size_t max, std::string& out) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec || size > max) return false;
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(size_t(size), '\0');
  f.read(out.data(), std::streamsize(size));
  return bool(f) || f.gcount() == std::streamsize(size);
}

template <typename T>
bool ParseNumber(const std::string& v, T& out) {
  const char* end = v.data() + v.size();
  auto r = std::from_chars(v.data(), end, out);
  return r.ec == std::errc() && r.ptr == end;
}

// Whether the registry knows the setting and the value suits its type and limits.
SettingCheck CheckSetting(const std::string& name, const std::string& value) {
  const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(name);
  if (!info || info->type == rex::cvar::FlagType::Command) return SettingCheck::kUnknown;
  double number = 0;
  bool numeric = false;
  switch (info->type) {
    case rex::cvar::FlagType::Boolean:
      if (value != "true" && value != "false") return SettingCheck::kBadValue;
      break;
    case rex::cvar::FlagType::Int32:
    case rex::cvar::FlagType::Int64: {
      int64_t n = 0;
      if (!ParseNumber(value, n)) return SettingCheck::kBadValue;
      number = double(n), numeric = true;
      break;
    }
    case rex::cvar::FlagType::Uint32:
    case rex::cvar::FlagType::Uint64: {
      uint64_t n = 0;
      if (!ParseNumber(value, n)) return SettingCheck::kBadValue;
      number = double(n), numeric = true;
      break;
    }
    case rex::cvar::FlagType::Double: {
      char* end = nullptr;
      number = std::strtod(value.c_str(), &end);
      if (value.empty() || !end || *end) return SettingCheck::kBadValue;
      numeric = true;
      break;
    }
    default: break;
  }
  const auto& c = info->constraints;
  if (numeric && ((c.min && number < *c.min) || (c.max && number > *c.max))) return SettingCheck::kBadValue;
  if (c.HasAllowedValues() &&
      std::find(c.allowed_values.begin(), c.allowed_values.end(), value) == c.allowed_values.end())
    return SettingCheck::kBadValue;
  return SettingCheck::kOk;
}

bool Inside(const fs::path& file, const fs::path& dir) {
  std::error_code ec;
  const fs::path a = fs::weakly_canonical(file, ec), b = fs::weakly_canonical(dir, ec);
  if (ec) return false;
  const auto rel = a.lexically_relative(b);
  return !rel.empty() && *rel.begin() != "..";
}

void CheckFolder(Mod& m) {
  std::error_code ec;
  if (!m.picture.empty()) {
    std::string ext = Lower(m.picture.extension().string());
    const auto size = fs::file_size(m.picture, ec);
    if (ec || size > 8u * 1024 * 1024 || (ext != ".png" && ext != ".jpg" && ext != ".jpeg") ||
        !Inside(m.picture, m.dir)) {
      m.issues.push_back({Problem::kNoPicture, U8(m.picture.filename())});
      m.picture.clear();
    }
  }
  if (m.type == Type::kTexturePack) {
    for (const fs::path& dir : {m.dir / "textures", m.dir}) {
      if (fs::is_regular_file(dir / "pack.txt", ec)) {
        m.content = dir;
        break;
      }
    }
    if (m.content.empty()) {
      m.issues.push_back({Problem::kNoTextures, {}});
      return;
    }
    std::string text, why;
    if (!ReadSmall(m.content / "pack.txt", 8u << 20, text)) why = "missing/oversized pack.txt";
    else ReadPackKeys(text, m.texture_keys, why);
    if (!why.empty()) m.issues.push_back({Problem::kBadPack, why});
  } else if (m.type == Type::kCutscenes) {
    const fs::path movies = m.dir / "movies";
    for (fs::directory_iterator it(movies, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code type_ec;
      if (!it->is_regular_file(type_ec)) continue;
      const std::string name = Lower(U8(it->path().filename()));
      if (name.size() > 4 && name.compare(name.size() - 4, 4, ".mp4") == 0) m.clips.push_back(name);
      if (m.clips.size() > 500) break;
    }
    std::sort(m.clips.begin(), m.clips.end());
    if (m.clips.empty()) m.issues.push_back({Problem::kNoMovies, {}});
    else m.content = movies;
  }
}

void ScanRoot(const fs::path& root, bool builtin, std::vector<Mod>& out) {
  std::error_code ec;
  std::vector<fs::path> dirs;
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code type_ec;
    if (it->is_directory(type_ec)) dirs.push_back(it->path());
  }
  std::sort(dirs.begin(), dirs.end());
  for (const fs::path& dir : dirs) {
    if (out.size() >= kMaxMods) break;
    Mod m;
    m.folder = U8(dir.filename());
    if (!m.folder.empty() && m.folder[0] == '.') continue;  // hidden folders
    m.dir = dir;
    m.builtin = builtin;
    m.id = ModId(m.folder, builtin);
    m.name = m.folder;
    try {
      if (!ValidFolderName(m.folder)) {
        m.issues.push_back({Problem::kBadFolderName, m.folder});
      } else {
        std::string text;
        if (!fs::is_regular_file(dir / "mod.toml", ec)) m.issues.push_back({Problem::kNoToml, {}});
        else if (!ReadSmall(dir / "mod.toml", kMaxTomlBytes, text))
          m.issues.push_back({Problem::kBadToml, "unreadable or larger than 64 KB"});
        else {
          ParseModToml(m, text, CheckSetting);
          if (m.name.empty()) m.name = m.folder;
          CheckGameVersion(m, GameVersion());
          CheckFolder(m);
        }
      }
    } catch (const std::exception& e) {
      m.issues.push_back({Problem::kBadToml, e.what()});
    }
    out.push_back(std::move(m));
  }
}

}  // namespace

fs::path UserDir(const fs::path& user_data) { return user_data / "mods"; }
fs::path BuiltinDir() { return rex::filesystem::GetExecutableFolder() / "mods-builtin"; }
const char* GameVersion() { return KK_VERSION; }

std::vector<Mod> Scan(const fs::path& user_data) {
  std::vector<Mod> out;
  ScanRoot(BuiltinDir(), true, out);
  ScanRoot(UserDir(user_data), false, out);
  return out;
}

std::string GameplayDigest(const fs::path& user_data) {
  return GameplayFingerprint(Scan(user_data), ParseEnabled(REXCVAR_GET(kk_mods)),
      [](const std::string& name) { return rex::cvar::GetFlagByName(name); });
}

void ApplyAtGameStart(const fs::path& user_data) {
  std::vector<std::string> ids = ParseEnabled(REXCVAR_GET(kk_mods));
  if (ids.empty()) return;
  const std::vector<Mod> all = Scan(user_data);
  {
    // Task 209: one texture pack at a time. Settings that have several on (from an older build, or the settings
    // file edited by hand) keep the same one the launcher keeps (texture_packs_logic.h Normalize).
    const auto is_tex = [&](const std::string& id) {
      const auto it = std::find_if(all.begin(), all.end(), [&](const Mod& m) { return m.id == id; });
      return it != all.end() && it->type == Type::kTexturePack;
    };
    texpacks::Choice c{rex::cvar::GetFlagByName("native_texture_pack") == "true",
                       rex::cvar::GetFlagByName("native_texture_pack_folder"), ids};
    if (texpacks::CountOn(c, is_tex) > 1) {
      const texpacks::Choice n = texpacks::Normalize(c, is_tex);
      REXLOG_INFO("KK mods: several texture packs were on; keeping {}", texpacks::ActiveId(n, is_tex));
      ids = n.mods;
      rex::cvar::SetFlagByName("kk_mods", JoinEnabled(n.mods));
      rex::cvar::SetFlagByName("native_texture_pack", n.folder_on ? "true" : "false");
    }
  }
  for (const auto& id : ids) {
    const auto it = std::find_if(all.begin(), all.end(), [&](const Mod& m) { return m.id == id; });
    if (it == all.end()) REXLOG_WARN("KK mods: {} is switched on but was not found; skipped", id);
    else if (!it->Usable()) REXLOG_WARN("KK mods: {} cannot be used (see the Workshop); skipped", id);
  }
  const std::vector<const Mod*> enabled = EnabledInOrder(all, ids);
  for (const Conflict& c : FindConflicts(enabled))
    REXLOG_INFO("KK mods: {} and {} change {} of the same {} ({} wins)", c.earlier, c.later, c.count,
                c.type == Type::kSettings ? "settings" : c.type == Type::kCutscenes ? "clips" : "textures", c.later);
  const Plan plan = BuildPlan(enabled);
  for (size_t i = 0; i < plan.settings.size(); ++i) {
    const Setting& s = plan.settings[i];
    const auto source = rex::cvar::GetFlagSource(s.name);
    if (source == rex::cvar::Source::kCommandLine || source == rex::cvar::Source::kEnvironment) {
      REXLOG_INFO("KK mods: {} = {} from {} not applied: given on the command line", s.name, s.value,
                  plan.setting_sources[i]);
      continue;
    }
    if (SetModSetting(s.name, s.value))
      REXLOG_INFO("KK mods: {} = {} (from {})", s.name, s.value, plan.setting_sources[i]);
    else
      REXLOG_WARN("KK mods: {} = {} from {} was refused", s.name, s.value, plan.setting_sources[i]);
  }
  if (!plan.texture_folders.empty()) {
    const bool own_on = rex::cvar::GetFlagByName("native_texture_pack") == "true";
    const std::string list =
        TextureFolderList(own_on, rex::cvar::GetFlagByName("native_texture_pack_folder"), plan.texture_folders);
    SetModSetting("native_texture_pack_folder", list);
    SetModSetting("native_texture_pack", "true");
    REXLOG_INFO("KK mods: texture packs {}", list);
  }
  if (!plan.movie_folders.empty()) {
    for (const auto& f : plan.movie_folders) REXLOG_INFO("KK mods: cutscene folder {}", U8(f));
    movies::SetOverrideFolders(plan.movie_folders);
  }
  REXLOG_INFO("KK mods: {} of {} switched-on mods applied", enabled.size(), ids.size());
}

}  // namespace kk::mods
