// Kong mods (Beta 7, owner 17 Oct: types 1-3 only): the parts with no game, window or settings registry, so
// they can be tested (tests/mods_tests.cpp). mods.cpp scans the folders and applies the enabled mods.
//
// A mod is a folder with a mod.toml:
//   <user data>/mods/<folder>/mod.toml      the player's mods
//   <program folder>/mods-builtin/<folder>/  the mods that ship with the game ("Built-in")
// Types: texture-pack (a texture pack in textures/ with its pack.txt), settings (a [settings] table of
// setting = value, applied while enabled on top of the player's own settings, never saved), cutscenes (a
// movies/ folder whose clips play instead of the PS2 clips of the same name). MODDING.md is the guide.
//
// The enabled mods are one saved setting, kk_mods: their ids in load order, separated by "|". A player's
// mod's id is its folder name; a built-in's is "builtin:" + its folder name. Later in the load order wins.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

namespace kk::mods {

enum class Type { kUnknown, kTexturePack, kSettings, kCutscenes };

inline const char* TypeId(Type t) {
  switch (t) {
    case Type::kTexturePack: return "texture-pack";
    case Type::kSettings: return "settings";
    case Type::kCutscenes: return "cutscenes";
    default: return "unknown";
  }
}

// What is wrong with a mod (it cannot be enabled), or with one of its entries (that entry is skipped).
// The launcher shows each one translated (workshop_mods.cpp); `detail` is the file, setting or version.
enum class Problem {
  kNoToml,            // mod.toml is missing
  kBadToml,           // detail: the parser's message
  kNoName,
  kBadType,           // detail: the type given
  kNoTextures,        // texture-pack without textures/pack.txt (or pack.txt in the folder)
  kBadPack,           // detail: why pack.txt was not read
  kNoMovies,          // cutscenes without movies/*.mp4
  kNoSettings,        // settings without any setting in [settings]
  kGameTooOld,        // detail: min_game_version
  kGameTooNew,        // detail: max_game_version
  kBadFolderName,     // "|" or ":" in the folder name
  // Warnings (the mod still works):
  kNoPicture,         // detail: picture file not found, not a PNG/JPEG, or outside the mod folder
  kUnknownSetting,    // detail: setting name
  kBlockedSetting,    // detail: setting name (paths, online, updates, ...)
  kBadValue,          // detail: setting name
};
inline bool IsWarning(Problem p) { return p >= Problem::kNoPicture; }

struct Issue {
  Problem problem;
  std::string detail;
};

struct Setting {
  std::string name, value;
};

struct Mod {
  std::string id;              // kk_mods entry
  std::string folder;          // folder name
  std::filesystem::path dir;   // the mod's folder
  bool builtin = false;
  std::string name, author, version, description;
  std::filesystem::path picture;  // empty: none (a plain panel)
  Type type = Type::kUnknown;
  std::string type_text;          // as written
  std::string min_game, max_game;
  std::vector<Setting> settings;  // settings mods, in file order
  std::filesystem::path content;  // the texture pack's folder (with pack.txt) or the movies folder
  std::vector<std::string> texture_keys;  // texture identities the pack replaces (conflicts)
  std::vector<std::string> clips;         // clip file names, lower case (conflicts)
  std::vector<Issue> issues;

  bool Usable() const {
    return std::none_of(issues.begin(), issues.end(), [](const Issue& i) { return !IsWarning(i.problem); });
  }
};

constexpr size_t kMaxTomlBytes = 64 * 1024;
constexpr size_t kMaxMods = 200;

// ------------------------------------------------------------------ versions ---
// "2.0.0", "2.0.0-beta.7", "v2.0.0-beta7": the numbers, then a pre-release comes before the release of the
// same numbers (2.0.0-beta.7 < 2.0.0), and betas compare by their own numbers. <0, 0, >0 like strcmp.
inline int CompareVersions(std::string_view a, std::string_view b) {
  auto split = [](std::string_view v, std::vector<long>& main, std::vector<long>& pre, bool& has_pre) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.remove_prefix(1);
    const size_t dash = v.find('-');
    has_pre = dash != std::string_view::npos;
    auto numbers = [](std::string_view s, std::vector<long>& out) {
      long n = -1;
      for (char c : s) {
        if (c >= '0' && c <= '9') n = (n < 0 ? 0 : n) * 10 + (c - '0');
        else if (n >= 0) out.push_back(n), n = -1;
        if (n > 100000000) n = 100000000;
      }
      if (n >= 0) out.push_back(n);
    };
    numbers(v.substr(0, dash), main);
    if (has_pre) numbers(v.substr(dash + 1), pre);
  };
  std::vector<long> am, ap, bm, bp;
  bool apre = false, bpre = false;
  split(a, am, ap, apre);
  split(b, bm, bp, bpre);
  const size_t n = std::max(am.size(), bm.size());
  for (size_t i = 0; i < n; ++i) {
    const long x = i < am.size() ? am[i] : 0, y = i < bm.size() ? bm[i] : 0;
    if (x != y) return x < y ? -1 : 1;
  }
  if (apre != bpre) return apre ? -1 : 1;
  const size_t m = std::max(ap.size(), bp.size());
  for (size_t i = 0; i < m; ++i) {
    const long x = i < ap.size() ? ap[i] : 0, y = i < bp.size() ? bp[i] : 0;
    if (x != y) return x < y ? -1 : 1;
  }
  return 0;
}

// The game version against a mod's range (either end may be empty). Adds kGameTooOld / kGameTooNew.
inline void CheckGameVersion(Mod& m, std::string_view game) {
  if (game.empty()) return;
  if (!m.min_game.empty() && CompareVersions(game, m.min_game) < 0) m.issues.push_back({Problem::kGameTooOld, m.min_game});
  if (!m.max_game.empty() && CompareVersions(game, m.max_game) > 0) m.issues.push_back({Problem::kGameTooNew, m.max_game});
}

// ------------------------------------------------------------------ settings ---
// Settings a mod may never change: where files are, identity, online and update switches, the mod list itself,
// and the settings that record what the setup has done.
inline bool BlockedSetting(std::string_view name) {
  static const std::set<std::string, std::less<>> kBlocked = {
      "kk_mods", "kk_install_id", "kk_online_count", "kk_online_asked", "kk_check_updates", "kk_update_channel",
      "kk_discord_status", "kk_discord_app_id", "kk_setup_version", "kk_settings_version", "kk_hardware_tier",
      "kk_ps2_movie_folder", "native_texture_pack_folder", "game_data_root", "user_data_root", "cache_root",
      "log_file", "kk_achievement_sound_file", "gpu_plugin", "kk_launcher", "kk_skip_launcher", "kk_start",
      "kk_renderer", "kk_benchmark", "kk_benchmark_passes", "kk_benchmark_from", "kk_launcher_open",
      "kk_launcher_saved"};
  return kBlocked.contains(name);
}

// How a setting checks: the registry knows the setting and its type (mods.cpp asks rex::cvar).
enum class SettingCheck { kOk, kUnknown, kBadValue };
using SettingChecker = std::function<SettingCheck(const std::string& name, const std::string& value)>;

// ------------------------------------------------------------------ mod.toml ---
// UTF-8 text as a path.
inline std::filesystem::path FromU8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

inline bool SafeRelative(const std::string& p) {
  if (p.empty() || p.size() > 200) return false;
  const std::filesystem::path path = FromU8(p);
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
  for (const auto& part : path)
    if (part == "..") return false;
  return true;
}

// A path as UTF-8 text (C++20 u8string is char8_t).
inline std::string U8(const std::filesystem::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

inline std::string Lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return s;
}

// Reads mod.toml's text. The folder checks (picture, textures, movies) are ScanMod's.
inline void ParseModToml(Mod& m, std::string_view text, const SettingChecker& check) {
  toml::table tbl;
#if TOML_EXCEPTIONS
  try {
    tbl = toml::parse(text);
  } catch (const toml::parse_error& e) {
    std::ostringstream why;
    why << e.description() << " (line " << e.source().begin.line << ")";
    m.issues.push_back({Problem::kBadToml, why.str()});
    return;
  }
#else
  auto parsed = toml::parse(text);
  if (!parsed) {
    std::ostringstream why;
    why << parsed.error().description() << " (line " << parsed.error().source().begin.line << ")";
    m.issues.push_back({Problem::kBadToml, why.str()});
    return;
  }
  tbl = std::move(parsed).table();
#endif
  auto str = [&](const char* key) {
    if (auto v = tbl[key].value<std::string>()) return *v;
    if (auto v = tbl[key].value<int64_t>()) return std::to_string(*v);
    if (auto v = tbl[key].value<double>()) {
      std::ostringstream o;
      o << *v;
      return o.str();
    }
    return std::string();
  };
  auto clip = [](std::string s, size_t n) {
    if (s.size() > n) s.resize(n);
    return s;
  };
  m.name = clip(str("name"), 80);
  m.author = clip(str("author"), 60);
  m.version = clip(str("version"), 30);
  m.description = clip(str("description"), 2000);
  m.type_text = clip(str("type"), 40);
  m.min_game = clip(str("min_game_version"), 40);
  m.max_game = clip(str("max_game_version"), 40);
  const std::string picture = str("picture");
  if (!picture.empty()) {
    if (SafeRelative(picture)) m.picture = m.dir / FromU8(picture);
    else m.issues.push_back({Problem::kNoPicture, picture});
  }
  if (m.name.empty()) m.issues.push_back({Problem::kNoName, {}});
  const std::string type = Lower(m.type_text);
  if (type == "texture-pack" || type == "texture_pack" || type == "texturepack") m.type = Type::kTexturePack;
  else if (type == "settings") m.type = Type::kSettings;
  else if (type == "cutscenes" || type == "cutscene" || type == "movies") m.type = Type::kCutscenes;
  else m.issues.push_back({Problem::kBadType, m.type_text});

  if (m.type == Type::kSettings) {
    if (const toml::table* s = tbl["settings"].as_table()) {
      for (const auto& [key, node] : *s) {
        Setting set;
        set.name = std::string(key.str());
        if (auto b = node.value_exact<bool>()) set.value = *b ? "true" : "false";
        else if (auto i = node.value_exact<int64_t>()) set.value = std::to_string(*i);
        else if (auto d = node.value_exact<double>()) {
          std::ostringstream o;
          o << *d;
          set.value = o.str();
        } else if (auto t = node.value_exact<std::string>()) set.value = *t;
        else {
          m.issues.push_back({Problem::kBadValue, set.name});
          continue;
        }
        if (BlockedSetting(set.name)) {
          m.issues.push_back({Problem::kBlockedSetting, set.name});
          continue;
        }
        const SettingCheck c = check ? check(set.name, set.value) : SettingCheck::kOk;
        if (c == SettingCheck::kUnknown) {
          m.issues.push_back({Problem::kUnknownSetting, set.name});
          continue;
        }
        if (c == SettingCheck::kBadValue) {
          m.issues.push_back({Problem::kBadValue, set.name});
          continue;
        }
        m.settings.push_back(std::move(set));
        if (m.settings.size() >= 200) break;
      }
    }
    if (m.settings.empty()) m.issues.push_back({Problem::kNoSettings, {}});
  }
}

// pack.txt's texture identities (hash, original format and size: what the loader matches), or why not.
inline bool ReadPackKeys(std::string_view text, std::vector<std::string>& keys, std::string& why) {
  keys.clear();
  std::istringstream in{std::string(text)};
  std::string line;
  if (!std::getline(in, line) || (line != "KKTP 1" && line != "KKTP 1\r")) {
    why = "unsupported pack version";
    return false;
  }
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::istringstream row(line);
    std::string hash, name, format, w, h, file;
    if (!(row >> hash >> name >> format >> w >> h >> file)) {
      why = "invalid manifest row";
      return false;
    }
    keys.push_back(Lower(hash) + " " + format + " " + w + "x" + h);
    if (keys.size() > 32768) {
      why = "too many entries";
      return false;
    }
  }
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  return true;
}

// ---------------------------------------------------------------- load order ---
inline bool ValidFolderName(std::string_view folder) {
  return !folder.empty() && folder.find('|') == std::string_view::npos && folder.find(':') == std::string_view::npos &&
         folder[0] != '.';
}

// Built in mods that no longer ship (task 360, Beta 17: Ultrawide is built into the game). An update that only
// adds files may leave their folders behind in mods-builtin; they are never listed or loaded.
inline bool RetiredBuiltin(std::string_view folder) { return folder == "ultrawide"; }

inline std::string ModId(std::string_view folder, bool builtin) {
  return (builtin ? "builtin:" : "") + std::string(folder);
}

inline std::vector<std::string> ParseEnabled(std::string_view list) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= list.size()) {
    size_t end = list.find('|', start);
    if (end == std::string_view::npos) end = list.size();
    std::string id(list.substr(start, end - start));
    if (!id.empty() && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(std::move(id));
    start = end + 1;
  }
  return out;
}

inline std::string JoinEnabled(const std::vector<std::string>& ids) {
  std::string out;
  for (const auto& id : ids) out += (out.empty() ? "" : "|") + id;
  return out;
}

// Enabling adds the mod at the end of the load order (it wins over the ones before); disabling removes it.
inline void SetEnabled(std::vector<std::string>& ids, const std::string& id, bool on) {
  const auto it = std::find(ids.begin(), ids.end(), id);
  if (on && it == ids.end()) ids.push_back(id);
  if (!on && it != ids.end()) ids.erase(it);
}

// Moves an enabled mod `delta` places (negative: earlier). False when it cannot move.
inline bool Move(std::vector<std::string>& ids, const std::string& id, int delta) {
  const auto it = std::find(ids.begin(), ids.end(), id);
  if (it == ids.end() || delta == 0) return false;
  const long at = long(it - ids.begin()), to = std::clamp(at + delta, 0L, long(ids.size()) - 1);
  if (to == at) return false;
  std::string moving = *it;
  ids.erase(it);
  ids.insert(ids.begin() + to, std::move(moving));
  return true;
}

// The enabled mods that exist and can be used, in load order.
inline std::vector<const Mod*> EnabledInOrder(const std::vector<Mod>& all, const std::vector<std::string>& ids) {
  std::vector<const Mod*> out;
  for (const auto& id : ids)
    for (const auto& m : all)
      if (m.id == id && m.Usable()) {
        out.push_back(&m);
        break;
      }
  return out;
}

// ----------------------------------------------------------------- conflicts ---
// Two enabled mods of the same type that change the same thing: the later one wins.
struct Conflict {
  Type type = Type::kUnknown;
  std::string earlier, later;      // mod ids
  std::vector<std::string> items;  // the first few settings / clips / textures they share
  size_t count = 0;                // how many they share
};

inline std::vector<Conflict> FindConflicts(const std::vector<const Mod*>& enabled) {
  std::vector<Conflict> out;
  for (size_t j = 0; j < enabled.size(); ++j) {
    for (size_t i = 0; i < j; ++i) {
      const Mod& a = *enabled[i];
      const Mod& b = *enabled[j];
      if (a.type != b.type) continue;
      std::vector<std::string> left, right;
      if (a.type == Type::kSettings) {
        for (const auto& s : a.settings) left.push_back(s.name);
        for (const auto& s : b.settings) right.push_back(s.name);
      } else if (a.type == Type::kCutscenes) {
        left = a.clips;
        right = b.clips;
      } else if (a.type == Type::kTexturePack) {
        left = a.texture_keys;
        right = b.texture_keys;
      }
      std::sort(left.begin(), left.end());
      std::sort(right.begin(), right.end());
      std::vector<std::string> both;
      std::set_intersection(left.begin(), left.end(), right.begin(), right.end(), std::back_inserter(both));
      both.erase(std::unique(both.begin(), both.end()), both.end());
      if (both.empty()) continue;
      Conflict c;
      c.type = a.type;
      c.earlier = a.id;
      c.later = b.id;
      c.count = both.size();
      both.resize(std::min<size_t>(both.size(), 5));
      c.items = std::move(both);
      out.push_back(std::move(c));
    }
  }
  return out;
}

// --------------------------------------------------------------------- plan ---
// What the game does with the enabled mods at its start.
struct Plan {
  std::vector<Setting> settings;                    // one per setting, the last mod's value
  std::vector<std::string> setting_sources;         // the mod id each value came from
  std::vector<std::filesystem::path> texture_folders;  // load order (later wins)
  std::vector<std::filesystem::path> movie_folders;    // load order (later wins)
};

inline Plan BuildPlan(const std::vector<const Mod*>& enabled) {
  Plan p;
  for (const Mod* m : enabled) {
    switch (m->type) {
      case Type::kSettings:
        for (const auto& s : m->settings) {
          bool found = false;
          for (size_t i = 0; i < p.settings.size(); ++i)
            if (p.settings[i].name == s.name) {
              p.settings[i].value = s.value;
              p.setting_sources[i] = m->id;
              found = true;
            }
          if (!found) {
            p.settings.push_back(s);
            p.setting_sources.push_back(m->id);
          }
        }
        break;
      case Type::kTexturePack: p.texture_folders.push_back(m->content); break;
      case Type::kCutscenes: p.movie_folders.push_back(m->content); break;
      default: break;
    }
  }
  return p;
}

// native_texture_pack_folder with mods: the player's own pack (when it is on) first, then the mods' packs in
// load order, separated by "|" (the loader reads them in that order; a later pack's texture wins).
inline std::string TextureFolderList(bool own_on, const std::string& own, const std::vector<std::filesystem::path>& mods) {
  std::string out = own_on ? own : std::string();
  for (const auto& f : mods) {
    const std::string s = U8(f);
    out += (out.empty() ? "" : "|") + s;
  }
  return out;
}

// ------------------------------------------------------- settings overlay ---
// A settings mod's values are put over the player's own while it is enabled and are never saved: the saved
// file keeps what the player chose (settings.cpp SaveSettings uses this). Pure so it can be tested.
struct OverlayEntry {
  std::string saved;   // the player's value before the mod
  int saved_source = 0;  // its rex::cvar::Source
  std::string applied;   // the value the mod set (as the setting reads back)
};

// The value to save for a setting whose live value is `live`: the player's own while the mod's value is
// still in place; the live one once the player changed it (their choice, which they then keep).
inline const std::string& ValueToSave(const OverlayEntry* e, const std::string& live) {
  return e && live == e->applied ? e->saved : live;
}

}  // namespace kk::mods
