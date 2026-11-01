// See user_folder.h.
#include "user_folder.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <vector>

namespace kk::user_folder {

namespace {

constexpr uint64_t kCompareLimit = 4ull << 20;  // files up to this size (saves) are compared byte by byte

std::string U8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

std::string ReplaceAll(std::string text, std::string_view from, std::string_view to) {
  if (from.empty()) return text;
  for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
    text.replace(at, from.size(), to);
  return text;
}

std::string Spelled(std::string s, std::string_view separator) {
  return ReplaceAll(ReplaceAll(std::move(s), "\\", "/"), "/", separator);
}

bool ReadFile(const fs::path& file, std::string* out) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !in.bad();
}

bool WriteFile(const fs::path& file, const std::string& text) {
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out.write(text.data(), std::streamsize(text.size()));
  out.close();
  return bool(out);
}

// Replaces `file` only when it holds the old folder, through a temp file, so a cut leaves the old or the new text.
bool RewriteFile(const fs::path& file, const std::string& old_root, const std::string& new_root) {
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) return true;
  std::string text;
  if (!ReadFile(file, &text)) return false;
  const std::string changed = ReplaceRoot(text, old_root, new_root);
  if (changed == text) return true;
  fs::path temp = file;
  temp += ".moving";
  if (!WriteFile(temp, changed)) return false;
  fs::rename(temp, file, ec);
  if (ec) fs::remove(temp, ec);
  return !ec;
}

// The same file for the move's purposes. FAT and exFAT (a USB stick) keep times to 2 s, so the times may differ.
bool SameFile(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  const uint64_t size_a = fs::file_size(a, ec);
  if (ec) return false;
  const uint64_t size_b = fs::file_size(b, ec);
  if (ec || size_a != size_b) return false;
  const auto time_a = fs::last_write_time(a, ec);
  if (ec) return false;
  const auto time_b = fs::last_write_time(b, ec);
  if (ec) return false;
  const auto gap = time_a > time_b ? time_a - time_b : time_b - time_a;
  return gap <= std::chrono::seconds(3);
}

bool SameBytes(const fs::path& a, const fs::path& b) {
  std::string text_a, text_b;
  return ReadFile(a, &text_a) && ReadFile(b, &text_b) && text_a == text_b;
}

struct Scan {
  std::vector<fs::path> files;  // relative to the old folder
  uint64_t bytes = 0;
  bool cloud = false;
  bool ok = true;
};

Scan ScanFiles(const Options& o) {
  Scan s;
  std::error_code ec;
  if (!fs::exists(o.old_root, ec)) return s;  // a move cut short after the last rename
  fs::recursive_directory_iterator it(o.old_root, fs::directory_options::follow_directory_symlink, ec), end;
  if (ec) s.ok = false;
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code type_ec;
    if (!it->is_regular_file(type_ec)) continue;
    const uint64_t size = it->file_size(type_ec);
    if (type_ec) {
      s.ok = false;
      continue;
    }
    s.files.push_back(it->path().lexically_relative(o.old_root));
    s.bytes += size;
    if (o.cloud_file && o.cloud_file(it->path())) s.cloud = true;
  }
  if (ec) s.ok = false;
  return s;
}

bool HasFiles(const fs::path& folder) {
  std::error_code ec;
  fs::recursive_directory_iterator it(folder, fs::directory_options::follow_directory_symlink, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code type_ec;
    if (it->is_regular_file(type_ec)) return true;
  }
  return false;
}

bool Inside(const fs::path& folder, const fs::path& parent) {
  if (parent.empty()) return false;
  std::error_code ec;
  const fs::path rel = fs::weakly_canonical(folder, ec).lexically_relative(fs::weakly_canonical(parent, ec));
  return !rel.empty() && *rel.begin() != "..";
}

bool CanWrite(const fs::path& folder) {
  std::error_code ec;
  fs::create_directories(folder, ec);
  const fs::path probe = folder / ".kk-write-test";
  const bool ok = WriteFile(probe, "x");
  fs::remove(probe, ec);
  return ok;
}

class Mover {
 public:
  explicit Mover(const Options& o) : o_(o) {}

  void Log(const std::string& line) const {
    if (o_.log) o_.log(line);
  }

  // `resuming`: a file that differs from the old one is an unfinished copy of this move and is written again.
  // Otherwise the program folder has data of its own, which is never overwritten.
  bool MoveAll(const Scan& scan, bool resuming) {
    resuming_ = resuming;
    total_ = scan.bytes;
    const bool renames = o_.allow_rename && !scan.cloud && SameDrive();
    if (!renames && !EnoughSpace(scan)) return false;
    std::error_code ec;
    if (!fs::exists(o_.old_root, ec)) return true;
    std::vector<fs::path> top;
    for (fs::directory_iterator it(o_.old_root, ec), end; !ec && it != end; it.increment(ec)) top.push_back(it->path());
    if (ec) {
      Log("cannot list " + U8(o_.old_root) + ": " + ec.message());
      return false;
    }
    for (const auto& from : top) {
      const fs::path to = o_.new_root / from.filename();
      if (renames && !fs::exists(to, ec)) {
        fs::rename(from, to, ec);
        if (!ec) continue;
        Log("cannot rename " + U8(from) + " (" + ec.message() + "), copying it");
      }
      if (!CopyItem(from, to)) return false;
    }
    return true;
  }

  // Back to the old folder: what the renames took there (of this run or of one cut short).
  void PutBack() {
    std::error_code ec;
    std::vector<fs::path> top;
    for (fs::directory_iterator it(o_.new_root, ec), end; !ec && it != end; it.increment(ec)) top.push_back(it->path());
    for (const auto& from : top) {
      const fs::path name = from.filename();
      if (name == kMoving || fs::exists(o_.old_root / name, ec)) continue;
      fs::rename(from, o_.old_root / name, ec);
      if (ec) Log("cannot put back " + U8(name) + ": " + ec.message());
    }
  }

  bool RewritePaths() {
    const std::string from = U8(o_.old_root), to = U8(o_.new_root);
    bool ok = true;
    std::error_code ec;
    const fs::path content = o_.new_root / "content";
    fs::recursive_directory_iterator it(content, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      if (it.depth() >= 2) it.disable_recursion_pending();
      if (it->path().filename() == "installed.txt") ok = RewriteFile(it->path(), from, to) && ok;
    }
    return ok && (o_.config.empty() || RewriteFile(o_.config, from, to));  // last: the old folder is true until here
  }

  // Removes the files of the old folder that equal their copy, then the empty folders. True when nothing failed.
  // A file that does not equal its copy changed after the move or was not moved: it stays.
  bool CleanOld() {
    std::error_code ec;
    std::vector<fs::path> files, folders;
    fs::recursive_directory_iterator it(o_.old_root, fs::directory_options::follow_directory_symlink, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      std::error_code type_ec;
      if (it->is_directory(type_ec)) folders.push_back(it->path());
      else if (it->is_regular_file(type_ec)) files.push_back(it->path());
    }
    bool ok = true;
    size_t kept = 0;
    for (const auto& file : files) {
      if (!Copied(file, o_.new_root / file.lexically_relative(o_.old_root))) {
        ++kept;
        continue;
      }
      fs::remove(file, ec);
      if (ec) {
        Log("cannot remove " + U8(file) + ": " + ec.message());
        ok = false;
      }
    }
    for (auto folder = folders.rbegin(); folder != folders.rend(); ++folder) fs::remove(*folder, ec);  // empty ones
    fs::remove(o_.old_root, ec);
    if (kept) Log(std::to_string(kept) + " files stay in " + U8(o_.old_root) + ": they differ from the moved copy");
    return ok;
  }

 private:
  // Whether the old file may go: its copy is equal, or an installed.txt that differs only by the rewritten path.
  bool Copied(const fs::path& file, const fs::path& copy) const {
    if (file.filename() == "installed.txt") {
      std::string text, moved;
      return ReadFile(file, &text) && ReadFile(copy, &moved) &&
             ReplaceRoot(text, U8(o_.old_root), U8(o_.new_root)) == moved;
    }
    std::error_code ec;
    return SameFile(file, copy) && (fs::file_size(file, ec) > kCompareLimit || SameBytes(file, copy));
  }

  bool SameDrive() const { return o_.old_root.root_name() == o_.new_root.root_name(); }

  bool EnoughSpace(const Scan& scan) const {
    uint64_t need = 0;
    std::error_code ec;
    for (const auto& rel : scan.files) {
      const fs::path to = o_.new_root / rel;
      if (!fs::exists(to, ec) || !SameFile(o_.old_root / rel, to)) need += fs::file_size(o_.old_root / rel, ec);
    }
    const auto info = fs::space(o_.new_root, ec);
    const uint64_t room = o_.free_space ? o_.free_space(o_.new_root) : ec ? UINT64_MAX : info.available;
    if (room > need + (64ull << 20)) return true;
    Log("not enough room in " + U8(o_.new_root) + ": " + std::to_string(need >> 20) + " MB to move, " +
        std::to_string(room >> 20) + " MB free");
    return false;
  }

  bool CopyItem(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (!fs::is_directory(from, ec)) return CopyOne(from, to);
    fs::create_directories(to, ec);
    fs::recursive_directory_iterator it(from, fs::directory_options::follow_directory_symlink, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      const fs::path target = to / it->path().lexically_relative(from);
      std::error_code type_ec;
      if (it->is_directory(type_ec)) {
        fs::create_directories(target, type_ec);
      } else if (it->is_regular_file(type_ec) && !CopyOne(it->path(), target)) {
        return false;
      }
      if (type_ec) {
        Log("cannot read " + U8(it->path()) + ": " + type_ec.message());
        return false;
      }
    }
    if (ec) Log("cannot read " + U8(from) + ": " + ec.message());
    return !ec;
  }

  bool CopyOne(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    const uint64_t size = fs::file_size(from, ec);
    if (ec) return Fail("cannot read", from, ec);
    if (fs::exists(to, ec) && (!resuming_ || SameFile(from, to))) return Counted(size);
    fs::create_directories(to.parent_path(), ec);
    fs::path temp = to;
    temp += ".moving";
    if (!fs::copy_file(from, temp, fs::copy_options::overwrite_existing, ec)) return Fail("cannot copy", from, ec);
    const auto time = fs::last_write_time(from, ec);
    if (!ec) fs::last_write_time(temp, time, ec);
    if (ec) return Fail("cannot set the time of", temp, ec);
    if (!SameFile(from, temp) || (size <= kCompareLimit && !SameBytes(from, temp))) {
      fs::remove(temp, ec);
      Log("the copy of " + U8(from) + " is not equal to it");
      return false;
    }
    fs::rename(temp, to, ec);
    if (ec) return Fail("cannot finish", to, ec);
    return Counted(size);
  }

  bool Counted(uint64_t size) {
    done_ += size;
    if (o_.progress) o_.progress(done_, total_);
    return true;
  }

  bool Fail(const char* what, const fs::path& file, const std::error_code& ec) const {
    Log(std::string(what) + " " + U8(file) + ": " + ec.message());
    return false;
  }

  const Options& o_;
  bool resuming_ = false;
  uint64_t done_ = 0, total_ = 0;
};

}  // namespace

std::string StoredRoot(const std::string& path) {
  std::string plain = path;
  for (char& c : plain) c = c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
  const std::string mark = std::string("/") + kFolderName + "/";  // content\..., or mods\... for a Workshop pack
  const size_t at = plain.rfind(mark);
  return at == std::string::npos ? std::string() : path.substr(0, at + 1 + std::string_view(kFolderName).size());
}

void Repoint(const Options& o) {
  Mover mover(o);
  const std::string now = U8(o.new_root);
  std::error_code ec;
  std::vector<std::string> roots;
  for (fs::directory_iterator it(o.new_root / "content", ec), end; !ec && it != end; it.increment(ec)) {
    std::string text;
    if (!ReadFile(it->path() / "installed.txt", &text)) continue;
    const size_t at = text.find("folder=");
    if (at == std::string::npos) continue;
    const size_t stop = text.find_first_of("\r\n", at);
    const std::string root = StoredRoot(text.substr(at + 7, stop == std::string::npos ? stop : stop - at - 7));
    if (!root.empty() && root != now && std::find(roots.begin(), roots.end(), root) == roots.end())
      roots.push_back(root);
  }
  Options paths = o;
  paths.new_root = o.new_root;
  for (const std::string& root : roots) {
    paths.old_root = fs::path(std::u8string(reinterpret_cast<const char8_t*>(root.data()), root.size()));
    Mover(paths).RewritePaths();
    mover.Log("the King Kong folder moved: pack paths changed from " + root + " to " + now);
  }
}

std::string ReplaceRoot(const std::string& text, const std::string& old_root, const std::string& new_root) {
  std::string out = text;
  for (const std::string_view separator : {std::string_view("\\\\"), std::string_view("\\"), std::string_view("/")})
    out = ReplaceAll(std::move(out), Spelled(old_root, separator), Spelled(new_root, separator));
  return out;
}

bool Done(const fs::path& new_root) {
  std::error_code ec;
  return fs::exists(new_root / kDone, ec);
}

fs::path Resolve(const Options& o) {
  std::error_code ec;
  Mover mover(o);
  if (Inside(o.new_root, o.old_root)) {  // the game itself was put in Documents\king_kong: its data is already here
    mover.Log("the program folder is inside " + U8(o.old_root) + ", keeping it");
    return o.old_root;
  }
  const fs::path moving = o.new_root / kMoving, cleaning = o.new_root / kCleaning, done = o.new_root / kDone;
  if (fs::exists(done, ec)) return o.new_root;
  if (!CanWrite(o.new_root)) {
    mover.Log("cannot write to " + U8(o.new_root) + ", keeping " + U8(o.old_root));
    return o.old_root;
  }
  if (fs::exists(cleaning, ec)) {  // the move is over: only removing the old files is left
    if (mover.CleanOld()) fs::rename(cleaning, done, ec);
    return o.new_root;
  }
  const bool resuming = fs::exists(moving, ec);
  if (!resuming && (o.old_root.empty() || !HasFiles(o.old_root))) {
    WriteFile(done, "Nothing to move from Documents.\n");
    return o.new_root;
  }
  const Scan scan = ScanFiles(o);
  if (!scan.ok) {
    mover.Log("cannot read all of " + U8(o.old_root) + ", keeping it");
    return o.old_root;
  }
  mover.Log((resuming ? "carrying on moving " : "moving ") + std::to_string(scan.files.size()) + " files, " +
            std::to_string(scan.bytes >> 20) + " MB, from " + U8(o.old_root) + " to " + U8(o.new_root) +
            (scan.cloud ? " (some may be only in OneDrive, copying them)" : ""));
  if (!resuming && !WriteFile(moving, "Moving from " + U8(o.old_root) + "\n")) {
    mover.Log("cannot write " + U8(moving) + ", keeping " + U8(o.old_root));
    return o.old_root;
  }
  if (!mover.MoveAll(scan, resuming) || !mover.RewritePaths()) {
    mover.PutBack();
    mover.Log("the move stopped, keeping " + U8(o.old_root) + " and trying again at the next start");
    return o.old_root;
  }
  fs::rename(moving, cleaning, ec);
  if (ec) {
    mover.Log("cannot rename " + U8(moving) + ": " + ec.message());
    return o.new_root;  // settings already point here; the next start carries on and finds nothing left to copy
  }
  if (mover.CleanOld()) fs::rename(cleaning, done, ec);
  mover.Log("the move is finished");
  return o.new_root;
}

}  // namespace kk::user_folder
