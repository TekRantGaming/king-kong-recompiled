// This file is UTF-8.
#include "artbook_files.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <system_error>

#include "artbook_logic.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace kk::artbook {
namespace fs = std::filesystem;

namespace {

std::string U8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}
fs::path FromU8(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())); }

struct File {
  std::FILE* f = nullptr;
  File(const fs::path& p, bool write) {
#if defined(_WIN32)
    _wfopen_s(&f, p.c_str(), write ? L"wb" : L"rb");
#else
    f = std::fopen(p.c_str(), write ? "wb" : "rb");
#endif
  }
  ~File() {
    if (f) std::fclose(f);
  }
  bool Close() {
    const bool ok = f && std::fclose(f) == 0;
    f = nullptr;
    return ok;
  }
  explicit operator bool() const { return f != nullptr; }
};

bool ReadAll(const fs::path& p, std::vector<uint8_t>& out, uint64_t limit) {
  std::error_code ec;
  const auto size = fs::file_size(p, ec);
  if (ec || size > limit) return false;
  File f(p, false);
  if (!f) return false;
  out.resize(size_t(size));
  return size == 0 || std::fread(out.data(), 1, out.size(), f.f) == out.size();
}

bool SameBytes(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  const auto sa = fs::file_size(a, ec);
  if (ec) return false;
  const auto sb = fs::file_size(b, ec);
  if (ec || sa != sb) return false;
  File fa(a, false), fb(b, false);
  if (!fa || !fb) return false;
  std::vector<char> x(1 << 16), y(1 << 16);
  for (;;) {
    const size_t na = std::fread(x.data(), 1, x.size(), fa.f), nb = std::fread(y.data(), 1, y.size(), fb.f);
    if (na != nb || std::memcmp(x.data(), y.data(), na) != 0) return false;
    if (na == 0) return true;
  }
}

}  // namespace

bool ConvertToIso(const fs::path& raw, const fs::path& iso, const Progress& progress, std::string& why) {
  std::error_code ec;
  const uint64_t size = fs::file_size(raw, ec);
  if (ec) return why = "cannot read " + U8(raw), false;
  if (size == 0 || size % kRawSector) return why = "the disc image is not whole 2352-byte sectors", false;
  File in(raw, false);
  if (!in) return why = "cannot open " + U8(raw), false;
  File out(iso, true);
  if (!out) return why = "cannot write " + U8(iso), false;
  constexpr size_t kBlock = 446;  // sectors per read: about 1 MB
  std::vector<uint8_t> buf(kBlock * kRawSector), data(kBlock * kIsoSector);
  uint64_t done = 0, sector = 0;
  while (done < size) {
    const size_t n = size_t(std::min<uint64_t>(kBlock, (size - done) / kRawSector));
    if (std::fread(buf.data(), kRawSector, n, in.f) != n) return why = "read error at byte " + std::to_string(done), false;
    size_t bad = 0;
    SectorError e = SectorError::kNone;
    if (!ConvertSectors(buf.data(), n, data.data(), &bad, &e))
      return why = "sector " + std::to_string(sector + bad) +
                   (e == SectorError::kForm2 ? " is Mode 2 Form 2 (not data)" : " has no CD sector header"),
             false;
    if (std::fwrite(data.data(), kIsoSector, n, out.f) != n) return why = "write error in " + U8(iso), false;
    done += uint64_t(n) * kRawSector;
    sector += n;
    if (progress && !progress(done)) return why = "stopped", false;
  }
  if (!out.Close()) return why = "write error in " + U8(iso), false;
  return true;
}

bool ReadBytes(const fs::path& file, std::vector<uint8_t>& out, uint64_t limit) {
  return ReadAll(file, out, limit);
}

std::vector<uint8_t> ReadHead(const fs::path& file, size_t n) {
  std::vector<uint8_t> head;
  File f(file, false);
  if (!f) return head;
  head.resize(n);
  head.resize(std::fread(head.data(), 1, head.size(), f.f));
  return head;
}

bool JpegHeaderOk(const fs::path& file) {
  const std::vector<uint8_t> head = ReadHead(file, 64 * 1024);
  int w = 0, h = 0;
  return JpegSize(head.data(), head.size(), w, h) && w <= 16384 && h <= 16384;
}

bool ExtractArtbook(const fs::path& iso, const fs::path& out, const PictureCheck& check, const Progress& progress,
                    Extracted& result, std::string& why) {
  result = {};
  File in(iso, false);
  if (!in) return why = "cannot open " + U8(iso), false;
  std::error_code ec;
  const uint64_t iso_size = fs::file_size(iso, ec);
  const uint64_t sectors = ec ? 0 : iso_size / kIsoSector;
  IsoReader reader([&](uint32_t lba, uint32_t count, uint8_t* dst) {
    if (uint64_t(lba) + count > sectors) return false;
#if defined(_WIN32)
    if (_fseeki64(in.f, int64_t(lba) * int64_t(kIsoSector), SEEK_SET) != 0) return false;
#else
    if (fseeko(in.f, off_t(lba) * off_t(kIsoSector), SEEK_SET) != 0) return false;
#endif
    return std::fread(dst, kIsoSector, count, in.f) == count;
  });
  if (!reader.Open(&why)) return false;
  const auto folder = reader.Find({std::begin(kArtFolder), std::end(kArtFolder)});
  if (!folder || !folder->directory) return why = "the disc has no Weta Collector's Artwork folder", false;
  std::vector<IsoEntry> entries;
  if (!reader.List(*folder, entries, &why)) return false;
  std::vector<IsoEntry> jpgs;
  for (const IsoEntry& e : entries)
    if (!e.directory && HasExtension(e.name, ".jpg")) jpgs.push_back(e);
  if (jpgs.empty()) return why = "no pictures in the Weta Collector's Artwork folder", false;
  if (jpgs.size() > 32) return why = "unexpectedly many pictures", false;
  std::sort(jpgs.begin(), jpgs.end(), [](const IsoEntry& a, const IsoEntry& b) { return a.name < b.name; });
  uint64_t done = 0;
  for (const IsoEntry& e : jpgs) {
    if (!SafeName(e.name)) return why = "unsafe file name on the disc: " + e.name, false;
    if (e.size == 0 || e.size > kMaxPictureBytes) return why = "unusual picture size: " + e.name, false;
    const fs::path dest = out / FromU8(e.name);
    {
      File f(dest, true);
      if (!f) return why = "cannot write " + U8(dest), false;
      bool stopped = false;
      const bool ok = reader.Read(e, [&](const uint8_t* p, size_t n) {
        if (std::fwrite(p, 1, n, f.f) != n) return false;
        done += n;
        if (progress && !progress(done)) {
          stopped = true;
          return false;
        }
        return true;
      });
      if (stopped) return why = "stopped", false;
      if (!ok || !f.Close()) return why = "cannot copy " + e.name + " from the disc", false;
    }
    if (!(check ? check(dest) : JpegHeaderOk(dest))) return why = "the picture " + e.name + " does not decode", false;
    result.pictures.push_back(dest);
  }
  // The readme: the first Word document (or text file) in Readme, as UTF-8 text. Optional.
  if (const auto readme_dir = reader.Find({std::begin(kReadmeFolder), std::end(kReadmeFolder)});
      readme_dir && reader.List(*readme_dir, entries)) {
    for (const IsoEntry& e : entries) {
      if (e.directory || e.size > kMaxDocBytes || !(HasExtension(e.name, ".doc") || HasExtension(e.name, ".txt")))
        continue;
      std::vector<uint8_t> bytes;
      bytes.reserve(e.size);
      if (!reader.Read(e, [&](const uint8_t* p, size_t n) {
            bytes.insert(bytes.end(), p, p + n);
            return true;
          }))
        continue;
      std::string text = HasExtension(e.name, ".doc") ? WordDocText(bytes) : std::string(bytes.begin(), bytes.end());
      if (text.empty()) continue;
      std::ofstream t(out / kReadmeFile, std::ios::binary | std::ios::trunc);
      t << text << '\n';
      result.readme = bool(t);
      break;
    }
  }
  return true;
}

std::vector<fs::path> Paintings(const fs::path& folder) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (it->is_regular_file(e2) && HasExtension(U8(it->path().filename()), ".jpg")) out.push_back(it->path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string ReadmeText(const fs::path& folder) {
  std::vector<uint8_t> bytes;
  if (!ReadAll(folder / kReadmeFile, bytes, 1 << 20)) return {};
  std::string s(bytes.begin(), bytes.end());
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

bool Installed(const fs::path& folder) { return !Paintings(folder).empty(); }

fs::path PicturesDir() {
#if defined(KK_DEV_TOOLS)
  if (const char* v = std::getenv("KK_DEV_ARTBOOK_PICTURES"); v && *v) return FromU8(v).make_preferred();
#endif
#if defined(_WIN32)
  PWSTR path = nullptr;
  fs::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, &path)) && path) out = path;
  if (path) CoTaskMemFree(path);
  if (out.empty()) {
    if (const wchar_t* profile = _wgetenv(L"USERPROFILE")) out = fs::path(profile) / L"Pictures";
  }
  return out;
#else
  const char* home = std::getenv("HOME");  // NOLINT (Linux only)
  if (!home || !*home) return {};
  std::string config;
  if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x == '/') config = x;
  else config = std::string(home) + "/.config";
  std::vector<uint8_t> bytes;
  ReadAll(fs::path(config) / "user-dirs.dirs", bytes, 1 << 16);
  return fs::path(XdgPicturesDir(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), home));
#endif
}

fs::path SaveFolder() {
  const fs::path dir = PicturesDir();
  return dir.empty() ? dir : dir / "King Kong Artbook";
}

Saved SavePicture(const fs::path& picture, const fs::path& folder) {
  Saved r;
  std::error_code ec;
  if (folder.empty()) return r.why = "no Pictures folder", r;
  if (!fs::is_regular_file(picture, ec)) return r.why = "the picture is missing", r;
  fs::create_directories(folder, ec);
  if (!fs::is_directory(folder, ec)) return r.why = "cannot make " + U8(folder), r;
  const std::string name = U8(picture.filename());
  for (int n = 1; n < 100; ++n) {
    const fs::path dest = folder / FromU8(NumberedName(name, n));
    if (fs::exists(dest, ec)) {
      if (SameBytes(dest, picture)) {
        r.ok = r.already = true;
        r.file = dest;
        return r;
      }
      continue;
    }
    fs::path part = dest;
    part += ".part";
    fs::remove(part, ec);
    fs::copy_file(picture, part, ec);
    if (!ec) fs::rename(part, dest, ec);
    if (ec) {
      fs::remove(part, ec);
      r.why = "cannot copy into " + U8(folder);
      return r;
    }
    r.ok = true;
    r.file = dest;
    return r;
  }
  r.why = "too many copies in " + U8(folder);
  return r;
}

fs::path AutoFile(const fs::path& user_dir) { return user_dir / "content" / "downloads" / "artbook.auto"; }

AutoState ReadAuto(const fs::path& user_dir) {
  AutoState a;
  std::ifstream in(AutoFile(user_dir), std::ios::binary);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line == "wanted=1") a.wanted = true;
    else if (line == "told=1") a.told = true;
    else if (line.rfind("fails=", 0) == 0) a.fails = std::clamp(std::atoi(line.c_str() + 6), 0, 1000);
  }
  return a;
}

bool WriteAuto(const fs::path& user_dir, const AutoState& a) {
  std::error_code ec;
  const fs::path file = AutoFile(user_dir);
  fs::create_directories(file.parent_path(), ec);
  fs::path tmp = file;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << "wanted=" << (a.wanted ? 1 : 0) << "\nfails=" << a.fails << "\ntold=" << (a.told ? 1 : 0) << "\n";
  }
  fs::rename(tmp, file, ec);
  return !ec;
}

void ClearAuto(const fs::path& user_dir) {
  std::error_code ec;
  fs::remove(AutoFile(user_dir), ec);
}

}  // namespace kk::artbook
