#include "update_files.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>

#include "content_logic.h"

namespace kk::update {
namespace {

std::string Utf8(const fs::path& p) {
  const auto u = p.generic_u8string();
  return std::string(u.begin(), u.end());
}

bool IsHex64(std::string_view s) {
  if (s.size() != 64) return false;
  for (char c : s)
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  return true;
}

void Note(const Log& log, const std::string& line) {
  if (log) log(line);
}

fs::path WithSuffix(fs::path p, const char* suffix) {
  p += suffix;
  return p;
}

// Removes `dir` and its parents up to (not including) `root` while they are empty.
void PruneEmpty(const fs::path& root, fs::path dir) {
  std::error_code ec;
  const fs::path stop = root.lexically_normal();
  while (!dir.empty() && dir.lexically_normal() != stop && dir.has_relative_path()) {
    if (!fs::is_directory(dir, ec) || !fs::is_empty(dir, ec)) return;
    if (!fs::remove(dir, ec)) return;
    dir = dir.parent_path();
  }
}

}  // namespace

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

const FileEntry* Manifest::Find(std::string_view path) const {
  const std::string key = Lower(path);
  for (const auto& f : files)
    if (Lower(f.path) == key) return &f;
  return nullptr;
}

bool SafePath(std::string_view path) {
  if (path.empty() || path.size() > 1024 || path.front() == '/') return false;
  for (char c : path)
    if (static_cast<unsigned char>(c) < 0x20 || c == '\\' || c == ':') return false;
  size_t start = 0;
  while (start <= path.size()) {
    const size_t end = std::min(path.find('/', start), path.size());
    const std::string_view part = path.substr(start, end - start);
    if (part.empty() || part == "." || part == "..") return false;
    start = end + 1;
  }
  return true;
}

std::optional<Manifest> ParseManifest(std::string_view text, std::string* error) {
  Manifest m;
  auto bad = [&](size_t line, const std::string& why) -> std::optional<Manifest> {
    if (error) *error = "line " + std::to_string(line) + ": " + why;
    return std::nullopt;
  };
  size_t line_no = 0;
  for (size_t at = 0; at < text.size();) {
    size_t end = text.find('\n', at);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(at, end - at);
    at = end + 1;
    ++line_no;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line_no == 1 && line.size() >= 3 && line.substr(0, 3) == "\xEF\xBB\xBF") line.remove_prefix(3);  // BOM
    if (line.empty() || line.front() == '#') continue;
    if (line.substr(0, 8) == "version ") {
      m.version = std::string(line.substr(8));
      continue;
    }
    const size_t s1 = line.find(' ');
    const size_t s2 = s1 == std::string_view::npos ? s1 : line.find(' ', s1 + 1);
    if (s2 == std::string_view::npos) return bad(line_no, "not \"<sha256> <size> <path>\"");
    FileEntry f;
    f.sha256 = Lower(line.substr(0, s1));
    const std::string size(line.substr(s1 + 1, s2 - s1 - 1));
    f.path = std::string(line.substr(s2 + 1));
    if (!IsHex64(f.sha256)) return bad(line_no, "not a sha256");
    if (size.empty() || size.size() > 15 || size.find_first_not_of("0123456789") != std::string::npos)
      return bad(line_no, "not a size");
    f.size = std::strtoull(size.c_str(), nullptr, 10);
    if (!SafePath(f.path)) return bad(line_no, "unsafe path " + f.path);
    m.files.push_back(std::move(f));
  }
  return m;
}

std::optional<Manifest> ReadManifest(const fs::path& file, std::string* error) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    if (error) *error = "cannot read";
    return std::nullopt;
  }
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return ParseManifest(text, error);
}

Protected::Protected(std::vector<std::string> extra) {
  for (auto& e : extra) {
    std::string p = Lower(e);
    std::replace(p.begin(), p.end(), '\\', '/');
    while (!p.empty() && p.back() == '/') p.pop_back();
    if (!p.empty()) extra_.push_back(p);
  }
}

bool Protected::operator()(std::string_view path) const {
  const std::string p = Lower(path);
  static const char* const kFolders[] = {"game",          "logs",    "user",        "userdata", "cache", "mods",
                                         "texture-packs", "content", "screenshots", "workshop", "save-backups"};
  static const char* const kFiles[] = {"king_kong.toml", "fpe.log"};
  const std::string top = p.substr(0, p.find('/'));
  for (const char* f : kFiles)
    if (p == f) return true;
  if (top.size() < p.size())  // inside a folder
    for (const char* d : kFolders)
      if (top == d) return true;
  for (const auto& e : extra_)
    if (p == e || (p.size() > e.size() && p.compare(0, e.size(), e) == 0 && p[e.size()] == '/')) return true;
  return false;
}

std::string SumFor(std::string_view sums, std::string_view file_name) {
  for (size_t at = 0; at < sums.size();) {
    size_t end = sums.find('\n', at);
    if (end == std::string_view::npos) end = sums.size();
    std::string_view line = sums.substr(at, end - at);
    at = end + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.size() < 66 || !IsHex64(line.substr(0, 64)) || line[64] != ' ') continue;
    std::string_view name = line.substr(65);
    if (!name.empty() && (name.front() == ' ' || name.front() == '*')) name.remove_prefix(1);
    if (name == file_name) return Lower(line.substr(0, 64));
  }
  return {};
}

std::string Sha256Of(const void* data, size_t size) {
  content::Sha256 h;
  h.Update(data, size);
  return h.HexDigest();
}

std::string FileSha256(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return {};
  content::Sha256 h;
  std::vector<char> buf(1 << 20);
  while (in) {
    in.read(buf.data(), std::streamsize(buf.size()));
    if (in.gcount() > 0) h.Update(buf.data(), size_t(in.gcount()));
  }
  if (in.bad()) return {};
  return h.HexDigest();
}

fs::path PathOf(const fs::path& root, std::string_view rel) {
  return root / fs::path(std::u8string(reinterpret_cast<const char8_t*>(rel.data()), rel.size()));
}

std::vector<std::string> MissingOrWrong(const fs::path& root, const Manifest& m, const Protected& keep,
                                        uint64_t hash_limit) {
  std::vector<std::string> out;
  for (const auto& f : m.files) {
    if (keep(f.path) || Lower(f.path) == kExtraFilesZip) continue;
    const fs::path p = PathOf(root, f.path);
    std::error_code ec;
    const bool file = fs::is_regular_file(p, ec);
    const uint64_t size = file ? fs::file_size(p, ec) : 0;
    if (!file || ec || size != f.size || (size <= hash_limit && FileSha256(p) != f.sha256)) out.push_back(f.path);
  }
  return out;
}

InstallResult InstallFiles(const fs::path& src_root, const fs::path& dst_root, const std::vector<std::string>& paths,
                           const Log& log) {
  std::error_code ec;
  std::vector<fs::path> created_dirs;  // made for the update: removed again if it is undone
  std::vector<fs::path> staged;        // <target>.new
  auto unstage = [&] {
    for (const auto& p : staged) fs::remove(p, ec);
    for (auto it = created_dirs.rbegin(); it != created_dirs.rend(); ++it) PruneEmpty(dst_root, *it);
  };
  // 1. Stage.
  for (const auto& rel : paths) {
    const fs::path src = PathOf(src_root, rel), dst = PathOf(dst_root, rel);
    if (!fs::is_regular_file(src, ec)) {
      unstage();
      Note(log, "install: not in the update: " + rel);
      return {Outcome::kNoSource, rel};
    }
    for (fs::path d = dst.parent_path(); !d.empty() && !fs::exists(d, ec) && d != dst_root; d = d.parent_path())
      created_dirs.insert(created_dirs.begin(), d);
    fs::create_directories(dst.parent_path(), ec);
    const fs::path tmp = WithSuffix(dst, ".new");
    if (!fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec)) {
      Note(log, "install: could not write " + rel + ".new: " + ec.message());
      unstage();
      return {Outcome::kWrite, rel, ec.message()};
    }
    staged.push_back(tmp);
  }
  // 2. Swap, with undo.
  struct Swapped {
    fs::path dst;
    bool had_old;
  };
  std::vector<Swapped> done;
  auto undo = [&] {
    for (auto it = done.rbegin(); it != done.rend(); ++it) {
      const fs::path old = WithSuffix(it->dst, ".old");
      std::error_code e;
      fs::remove(it->dst, e);
      if (e) {  // cannot happen for a file just put there, but never lose the old one: move the new aside
        fs::rename(it->dst, WithSuffix(it->dst, ".undo"), e);
      }
      if (it->had_old) {
        fs::rename(old, it->dst, e);
        if (e) Note(log, "install: UNDO could not put back " + Utf8(it->dst) + ": " + e.message());
      }
    }
    unstage();
  };
  for (size_t i = 0; i < paths.size(); ++i) {
    const fs::path dst = PathOf(dst_root, paths[i]), tmp = staged[i], old = WithSuffix(dst, ".old");
    const bool had = fs::exists(dst, ec);
    if (had) {
      if (fs::exists(old, ec)) fs::remove(old, ec);  // a copy an earlier update left (a later start removes them)
      fs::rename(dst, old, ec);
      if (ec) {
        Note(log, "install: " + paths[i] + " is in use (" + ec.message() + "): undoing the update");
        undo();
        return {Outcome::kInUse, paths[i]};
      }
    }
    fs::rename(tmp, dst, ec);
    if (ec) {
      Note(log, "install: could not put " + paths[i] + " in place (" + ec.message() + "): undoing the update");
      if (had) {
        std::error_code e;
        fs::rename(old, dst, e);
      }
      undo();
      return {Outcome::kInUse, paths[i]};
    }
    done.push_back({dst, had});
  }
  // 3. The old copies (in use ones stay for the next start).
  for (const auto& d : done)
    if (d.had_old) fs::remove(WithSuffix(d.dst, ".old"), ec);
  Note(log, "install: " + std::to_string(paths.size()) + " files installed");
  return {};
}

std::vector<std::string> RetireFiles(const fs::path& root, const Manifest& retired, const Manifest& current,
                                     const Protected& keep, const Log& log) {
  std::map<std::string, std::vector<const FileEntry*>> by_path;  // lower case path: its listed versions
  for (const auto& f : retired.files)
    if (!current.Find(f.path) && !keep(f.path) && Lower(f.path) != kFilesList) by_path[Lower(f.path)].push_back(&f);
  std::vector<std::string> removed;
  std::set<std::string> dirs;
  for (const auto& [key, versions] : by_path) {
    const std::string& rel = versions.front()->path;
    const fs::path p = PathOf(root, rel);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) continue;
    const uint64_t size = fs::file_size(p, ec);
    if (ec) continue;
    bool sized = false;
    for (const auto* v : versions) sized |= v->size == size;
    if (!sized) {
      Note(log, "retire: kept " + rel + " (changed)");
      continue;
    }
    const std::string sha = FileSha256(p);
    bool same = false;
    for (const auto* v : versions) same |= v->size == size && v->sha256 == sha;
    if (!same) {
      Note(log, "retire: kept " + rel + " (changed)");
      continue;
    }
    if (!fs::remove(p, ec)) {  // in use: renamed, so a later start removes it (RemoveOldCopies)
      std::error_code e;
      fs::rename(p, WithSuffix(p, ".old"), e);
      if (e) {
        Note(log, "retire: could not remove " + rel + ": " + ec.message());
        continue;
      }
    }
    removed.push_back(rel);
    dirs.insert(Utf8(p.parent_path()));
    Note(log, "retire: removed " + rel);
  }
  for (auto it = dirs.rbegin(); it != dirs.rend(); ++it)
    PruneEmpty(root, fs::path(std::u8string(reinterpret_cast<const char8_t*>(it->data()), it->size())));
  return removed;
}

void RemoveOldCopies(const fs::path& root, const Manifest& m) {
  std::error_code ec;
  for (const auto& f : m.files) {
    const fs::path old = WithSuffix(PathOf(root, f.path), ".old");
    if (fs::is_regular_file(old, ec)) fs::remove(old, ec);
  }
}

SourceResult InstallFromSource(const fs::path& src_root, const fs::path& dst_root, const Manifest& m,
                               const std::vector<std::string>& needed, const Log& log) {
  SourceResult r;
  std::vector<std::string> ok;
  for (const auto& rel : needed) {
    const FileEntry* f = m.Find(rel);
    const fs::path src = PathOf(src_root, rel);
    std::error_code ec;
    if (f && fs::is_regular_file(src, ec) && fs::file_size(src, ec) == f->size && FileSha256(src) == f->sha256) {
      ok.push_back(f->path);
    } else {
      r.unavailable.push_back(rel);
      Note(log, "finish: the download has no matching copy of " + rel);
    }
  }
  if (!ok.empty()) r.install = InstallFiles(src_root, dst_root, ok, log);
  if (r.install.outcome == Outcome::kOk) r.installed = ok;
  return r;
}

std::vector<std::string> Unlisted(const fs::path& root, const Manifest& m, const Protected& keep) {
  std::set<std::string> listed;
  for (const auto& f : m.files) listed.insert(Lower(f.path));
  std::vector<std::string> out;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string rel = Utf8(fs::relative(it->path(), root, ec));
    const std::string key = Lower(rel);
    if (keep(rel) || key == kFilesList || listed.count(key)) continue;
    if (key.size() > 4 && key.compare(key.size() - 4, 4, ".old") == 0) continue;
    out.push_back(rel);
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace kk::update
