// The pure parts of the content downloads (content.h): SHA-256, HTTP range replies, the ZIP central
// directory, unsafe names (zip-slip), the texture pack and cutscene layouts, free space. No files, no
// network, no SDK: the tests (tests/content_tests.cpp) check all of it on Windows and Linux.
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace kk::content {

// ------------------------------------------------------------------ SHA-256 ---
class Sha256 {
 public:
  Sha256() { Reset(); }
  void Reset() {
    static constexpr uint32_t kInit[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h_, kInit, sizeof(h_));
    length_ = 0;
    used_ = 0;
  }
  void Update(const void* data, size_t size) {
    auto p = static_cast<const uint8_t*>(data);
    length_ += size;
    if (used_) {
      const size_t take = std::min(size, size_t(64) - used_);
      std::memcpy(block_ + used_, p, take);
      used_ += take;
      p += take;
      size -= take;
      if (used_ < 64) return;
      Compress(block_);
      used_ = 0;
    }
    for (; size >= 64; p += 64, size -= 64) Compress(p);
    std::memcpy(block_, p, size);
    used_ = size;
  }
  // The digest as 64 lowercase hex digits. Resets the object.
  std::string HexDigest() {
    const uint64_t bits = length_ * 8;
    const uint8_t pad = 0x80;
    Update(&pad, 1);
    const uint8_t zero = 0;
    while (used_ != 56) Update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
    Update(len, 8);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (uint32_t v : h_)
      for (int i = 28; i >= 0; i -= 4) out.push_back(kHex[(v >> i) & 15]);
    Reset();
    return out;
  }

 private:
  static uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void Compress(const uint8_t* b) {
    static constexpr uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = uint32_t(b[4 * i]) << 24 | uint32_t(b[4 * i + 1]) << 16 | uint32_t(b[4 * i + 2]) << 8 | b[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], bb = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 = h + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = bb;
      bb = a;
      a = t1 + t2;
    }
    h_[0] += a; h_[1] += bb; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
  }
  uint32_t h_[8];
  uint64_t length_ = 0;
  uint8_t block_[64];
  size_t used_ = 0;
};

// -------------------------------------------------------------- HTTP ranges ---
// The download server answers HEAD with 404, so the size comes from a GET of bytes=0-0 (its
// Content-Range "bytes 0-0/<total>"), and a download continues with "Range: bytes=<have>-".
inline std::string RangeValue(uint64_t from) { return "bytes=" + std::to_string(from) + "-"; }

struct ContentRange {
  bool ok = false;
  bool unsatisfied = false;  // "bytes */<total>" (a 416 reply)
  uint64_t first = 0, last = 0, total = 0;
};

inline ContentRange ParseContentRange(std::string_view v) {
  ContentRange r;
  while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
  if (v.size() < 6) return r;
  std::string unit(v.substr(0, 6));
  for (char& c : unit) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (unit != "bytes ") return r;
  v.remove_prefix(6);
  auto number = [&](uint64_t& out) {
    if (v.empty() || !std::isdigit(static_cast<unsigned char>(v.front()))) return false;
    out = 0;
    while (!v.empty() && std::isdigit(static_cast<unsigned char>(v.front()))) {
      if (out > (UINT64_MAX - 9) / 10) return false;
      out = out * 10 + uint64_t(v.front() - '0');
      v.remove_prefix(1);
    }
    return true;
  };
  if (!v.empty() && v.front() == '*') {
    v.remove_prefix(1);
    r.unsatisfied = true;
  } else {
    if (!number(r.first) || v.empty() || v.front() != '-') return r;
    v.remove_prefix(1);
    if (!number(r.last) || r.last < r.first) return r;
  }
  if (v.empty() || v.front() != '/') return r;
  v.remove_prefix(1);
  if (!number(r.total)) return r;  // "*" (unknown length) is not accepted: the size is needed
  if (!r.unsatisfied && r.last >= r.total) return r;
  r.ok = true;
  return r;
}

// What to do with the reply to "Range: bytes=<have>-".
enum class RangeReply {
  kAppend,    // 206 from <have> (or a 200 when nothing is kept yet): write on after the kept bytes
  kRestart,   // the server sent the whole file (200): start the partial file again from 0
  kComplete,  // 416 and the kept bytes are already the whole file
  kFail,      // anything else
};
inline RangeReply DecideRangeReply(uint64_t have, int status, const ContentRange& range) {
  if (status == 206) return range.ok && !range.unsatisfied && range.first == have ? RangeReply::kAppend
                                                                                  : RangeReply::kFail;
  if (status == 200) return have == 0 ? RangeReply::kAppend : RangeReply::kRestart;
  if (status == 416) return range.ok && range.unsatisfied && range.total == have && have > 0 ? RangeReply::kComplete
                                                                                            : RangeReply::kRestart;
  return RangeReply::kFail;
}

// The size from the reply to a GET of bytes=0-0: the Content-Range total (206), else Content-Length
// (a server without ranges sends the whole file with 200). 0: unknown.
inline uint64_t SizeFromProbe(int status, const ContentRange& range, uint64_t content_length) {
  if (status == 206 && range.ok && !range.unsatisfied) return range.total;
  if (status == 200) return content_length;
  return 0;
}

// --------------------------------------------------------------------- ZIP ---
struct ZipEntry {
  std::string name;  // as stored ('/' separators)
  uint64_t size = 0, packed = 0;
  uint32_t crc = 0;
  uint16_t method = 0;
  bool directory = false;
  bool link = false;  // a symbolic link (Unix mode in the external attributes)
};

// Rejects a name that could write outside the folder it is unpacked into, or that Windows would
// change: absolute paths, drive letters, "..", backslashes, colons (also NTFS streams), control
// characters, empty parts, parts ending in a dot or space, and reserved device names.
inline bool SafeEntryName(std::string_view name) {
  if (name.empty() || name.size() > 400) return false;
  if (name.front() == '/') return false;
  std::string_view rest = name;
  if (rest.back() == '/') rest.remove_suffix(1);  // a directory entry
  if (rest.empty()) return false;
  for (const char c : rest)
    if (static_cast<unsigned char>(c) < 0x20 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
        c == '<' || c == '>' || c == '|' || c == 0x7f)
      return false;
  size_t start = 0;
  while (start <= rest.size()) {
    const size_t slash = rest.find('/', start);
    const std::string_view part = rest.substr(start, slash == std::string_view::npos ? rest.npos : slash - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (part.back() == '.' || part.back() == ' ') return false;
    std::string stem(part.substr(0, part.find('.')));
    for (char& c : stem) c = char(std::toupper(static_cast<unsigned char>(c)));
    static const char* const kDevices[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
                                           "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
                                           "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    for (const char* d : kDevices)
      if (stem == d) return false;
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return true;
}

// Reads the central directory (with Zip64) through read_at(offset, buffer, size) -> bool. The
// entries are checked against the names only later (CheckPackLayout, ChooseMovieFolder).
template <typename ReadAt>
bool ReadZipDirectory(ReadAt&& read_at, uint64_t file_size, std::vector<ZipEntry>& out, std::string& error) {
  out.clear();
  auto u16 = [](const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); };
  auto u32 = [](const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; };
  auto u64 = [&](const uint8_t* p) { return uint64_t(u32(p)) | uint64_t(u32(p + 4)) << 32; };
  if (file_size < 22) return error = "too small for a ZIP file", false;
  // The end of central directory record is in the last 22 + 65535 bytes.
  const uint64_t tail_size = std::min<uint64_t>(file_size, 22 + 65535 + 20);
  std::vector<uint8_t> tail(static_cast<size_t>(tail_size));
  if (!read_at(file_size - tail_size, tail.data(), tail.size())) return error = "cannot read the file", false;
  int64_t eocd = -1;
  for (int64_t i = int64_t(tail.size()) - 22; i >= 0; --i)
    if (u32(&tail[size_t(i)]) == 0x06054b50) {
      eocd = i;
      break;
    }
  if (eocd < 0) return error = "not a ZIP file", false;
  const uint8_t* e = &tail[size_t(eocd)];
  uint64_t count = u16(e + 10), dir_size = u32(e + 12), dir_offset = u32(e + 16);
  if (u16(e + 4) != 0 || u16(e + 6) != 0) return error = "split ZIP files are not supported", false;
  if (count == 0xFFFF || dir_size == 0xFFFFFFFF || dir_offset == 0xFFFFFFFF) {
    // Zip64: the locator sits right before the end record.
    if (eocd < 20 || u32(e - 20) != 0x07064b50) return error = "missing Zip64 locator", false;
    const uint64_t rec = u64(e - 20 + 8);
    uint8_t z[56];
    if (rec + 56 > file_size || !read_at(rec, z, 56) || u32(z) != 0x06064b50)
      return error = "bad Zip64 end record", false;
    count = u64(z + 32);
    dir_size = u64(z + 40);
    dir_offset = u64(z + 48);
  }
  if (count > 200000 || dir_size > (64ull << 20) || dir_offset > file_size || dir_size > file_size - dir_offset)
    return error = "bad central directory", false;
  std::vector<uint8_t> dir(static_cast<size_t>(dir_size));
  if (dir_size && !read_at(dir_offset, dir.data(), dir.size())) return error = "cannot read the directory", false;
  size_t at = 0;
  for (uint64_t i = 0; i < count; ++i) {
    if (at + 46 > dir.size() || u32(&dir[at]) != 0x02014b50) return error = "bad directory entry", false;
    const uint8_t* c = &dir[at];
    const uint16_t made_by = u16(c + 4), flags = u16(c + 8);
    const size_t name_len = u16(c + 28), extra_len = u16(c + 30), comment_len = u16(c + 32);
    if (at + 46 + name_len + extra_len + comment_len > dir.size()) return error = "bad directory entry", false;
    ZipEntry entry;
    entry.method = u16(c + 10);
    entry.crc = u32(c + 16);
    entry.packed = u32(c + 20);
    entry.size = u32(c + 24);
    uint64_t local = u32(c + 42);
    entry.name.assign(reinterpret_cast<const char*>(c + 46), name_len);
    if (flags & 1) return error = "encrypted ZIP files are not supported", false;
    // Zip64 sizes and offset, in the order of the fields that were 0xFFFFFFFF.
    const uint8_t* x = c + 46 + name_len;
    for (size_t k = 0; k + 4 <= extra_len;) {
      const uint16_t id = u16(x + k), len = u16(x + k + 2);
      if (k + 4 + len > extra_len) break;
      if (id == 0x0001) {
        size_t f = k + 4;
        auto take = [&](uint64_t& v) {
          if (f + 8 <= k + 4 + len) {
            v = u64(x + f);
            f += 8;
          }
        };
        if (entry.size == 0xFFFFFFFF) take(entry.size);
        if (entry.packed == 0xFFFFFFFF) take(entry.packed);
        if (local == 0xFFFFFFFF) take(local);
      }
      k += 4 + len;
    }
    entry.directory = !entry.name.empty() && entry.name.back() == '/';
    const uint32_t attributes = u32(c + 38);
    const uint32_t mode = attributes >> 16;
    entry.link = (made_by >> 8) == 3 && (mode & 0170000) == 0120000;
    if (local >= file_size) return error = "bad entry offset", false;
    out.push_back(std::move(entry));
    at += 46 + name_len + extra_len + comment_len;
  }
  return true;
}

// ------------------------------------------------------------ the layouts ---
enum class Problem {
  kNone,
  kNotZip,          // not a ZIP file we can read
  kUnsafeName,      // a name that could write outside the folder (zip-slip), or a link
  kNoManifest,      // no pack.txt
  kManyManifests,   // more than one pack.txt
  kSubfolders,      // folders inside the pack folder
  kTooLarge,        // a file or the whole pack is larger than the loader accepts
  kBadManifest,     // pack.txt does not start with "KKTP 1" or a row is malformed
  kMissingFile,     // a file pack.txt lists is not in the pack
  kNoMovies,        // no BK_xxxx.mp4
};

// The loader's limits (native-renderer/docs/texture-packs.md): 32,768 rows, an 8 MiB manifest,
// 128 MiB per stored image. A little room for the container's headers.
constexpr uint64_t kMaxManifestBytes = 8ull << 20;
constexpr size_t kMaxRows = 32768;
constexpr uint64_t kMaxImageBytes = (128ull << 20) + (1ull << 20);
constexpr uint64_t kMaxPackBytes = 64ull << 30;

struct Layout {
  Problem problem = Problem::kNone;
  std::string folder;       // inside the ZIP, without the trailing '/' ("" for the top)
  uint64_t unpacked = 0;    // bytes of every file in the ZIP (what unpacking writes)
  size_t files = 0;         // files in that folder
};

inline bool EndsWithNoCase(std::string_view s, std::string_view suffix) {
  if (s.size() < suffix.size()) return false;
  for (size_t i = 0; i < suffix.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i])) !=
        std::tolower(static_cast<unsigned char>(suffix[i])))
      return false;
  return true;
}
inline std::string ParentOf(std::string_view name) {
  if (!name.empty() && name.back() == '/') name.remove_suffix(1);
  const size_t slash = name.rfind('/');
  return slash == std::string_view::npos ? std::string() : std::string(name.substr(0, slash));
}
inline std::string LeafOf(std::string_view name) {
  if (!name.empty() && name.back() == '/') name.remove_suffix(1);
  const size_t slash = name.rfind('/');
  return std::string(slash == std::string_view::npos ? name : name.substr(slash + 1));
}

// Names that are safe and no links: the first check of any ZIP before it is unpacked.
inline Layout CheckNames(const std::vector<ZipEntry>& entries) {
  Layout layout;
  if (entries.empty()) return layout.problem = Problem::kNotZip, layout;
  for (const auto& e : entries) {
    if (!SafeEntryName(e.name) || e.link) return layout.problem = Problem::kUnsafeName, layout;
    layout.unpacked += e.size;
  }
  return layout;
}

// A texture pack ZIP: exactly one pack.txt; its folder holds the images directly (no subfolders).
// Files elsewhere in the ZIP are not part of the pack and are not installed.
inline Layout CheckPackLayout(const std::vector<ZipEntry>& entries) {
  Layout layout = CheckNames(entries);
  if (layout.problem != Problem::kNone) return layout;
  const ZipEntry* manifest = nullptr;
  for (const auto& e : entries)
    if (!e.directory && LeafOf(e.name) == "pack.txt") {
      if (manifest) return layout.problem = Problem::kManyManifests, layout;
      manifest = &e;
    }
  if (!manifest) return layout.problem = Problem::kNoManifest, layout;
  if (manifest->size > kMaxManifestBytes) return layout.problem = Problem::kTooLarge, layout;
  layout.folder = ParentOf(manifest->name);
  const std::string prefix = layout.folder.empty() ? "" : layout.folder + "/";
  for (const auto& e : entries) {
    if (e.name.compare(0, prefix.size(), prefix) != 0) continue;  // outside the pack folder
    const std::string_view rest = std::string_view(e.name).substr(prefix.size());
    if (rest.empty()) continue;  // the folder's own entry
    if (rest.find('/') != std::string_view::npos) return layout.problem = Problem::kSubfolders, layout;
    if (e.size > kMaxImageBytes && &e != manifest) return layout.problem = Problem::kTooLarge, layout;
    ++layout.files;
  }
  if (layout.unpacked > kMaxPackBytes) layout.problem = Problem::kTooLarge;
  return layout;
}

// pack.txt (UTF-8 without BOM, LF or CRLF): first line "KKTP 1"; '#' lines and blank lines skipped;
// rows of six fields whose last is a file name in the pack folder (no path), which must exist.
inline Problem CheckManifest(std::string_view text, const std::function<bool(const std::string&)>& has_file,
                             size_t* rows_out = nullptr) {
  if (text.size() > kMaxManifestBytes) return Problem::kTooLarge;
  size_t rows = 0;
  bool first = true;
  std::set<std::string> seen;
  while (!text.empty()) {
    const size_t nl = text.find('\n');
    std::string_view line = text.substr(0, nl);
    text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (first) {
      if (line != "KKTP 1") return Problem::kBadManifest;
      first = false;
      continue;
    }
    size_t lead = 0;
    while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
    line.remove_prefix(lead);
    if (line.empty() || line.front() == '#') continue;
    std::vector<std::string_view> fields;
    for (size_t i = 0; i < line.size();) {
      while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
      const size_t start = i;
      while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
      if (i > start) fields.push_back(line.substr(start, i - start));
    }
    if (fields.size() != 6) return Problem::kBadManifest;
    const std::string file(fields[5]);
    if (file.find('/') != std::string::npos || !SafeEntryName(file)) return Problem::kBadManifest;
    if (!EndsWithNoCase(file, ".dds") && !EndsWithNoCase(file, ".png")) return Problem::kBadManifest;
    if (++rows > kMaxRows) return Problem::kTooLarge;
    if (seen.insert(file).second && !has_file(file)) return Problem::kMissingFile;
  }
  if (first) return Problem::kBadManifest;
  if (rows_out) *rows_out = rows;
  return Problem::kNone;
}

// BK_xxxx.mp4 (four hex digits), the names the game asks for (ps2_movies_logic.h MovieName).
inline bool IsMovieName(std::string_view leaf) {
  if (leaf.size() != 11 || !EndsWithNoCase(leaf, ".mp4")) return false;
  if (std::toupper(static_cast<unsigned char>(leaf[0])) != 'B' || std::toupper(static_cast<unsigned char>(leaf[1])) != 'K' ||
      leaf[2] != '_')
    return false;
  for (int i = 3; i < 7; ++i)
    if (!std::isxdigit(static_cast<unsigned char>(leaf[size_t(i)]))) return false;
  return true;
}

// The cutscene ZIP: the folder with the most BK_xxxx.mp4 files is the movie folder (KK_Cuts_AI.zip has
// KK_Cuts_AI/upscaled_only with all of them, and AI_expanded with one alternative clip).
inline Layout ChooseMovieFolder(const std::vector<ZipEntry>& entries) {
  Layout layout = CheckNames(entries);
  if (layout.problem != Problem::kNone) return layout;
  std::map<std::string, size_t> counts;
  for (const auto& e : entries)
    if (!e.directory && IsMovieName(LeafOf(e.name))) ++counts[ParentOf(e.name)];
  size_t best = 0;
  for (const auto& [folder, n] : counts)
    if (n > best) {
      best = n;
      layout.folder = folder;
    }
  if (!best) layout.problem = Problem::kNoMovies;
  layout.files = best;
  return layout;
}

// ------------------------------------------------------------ free space ---
// Room kept free beyond what the download and unpacking write.
constexpr uint64_t kSpaceMargin = 256ull << 20;
// Space a download still needs: the rest of the ZIP, plus everything it unpacks to (the ZIP is deleted
// only after unpacking), plus the margin.
inline uint64_t SpaceNeeded(uint64_t zip_size, uint64_t have, uint64_t unpacked) {
  return (zip_size > have ? zip_size - have : 0) + unpacked + kSpaceMargin;
}

// Seconds until done at this speed (bytes per second); negative when unknown.
inline double SecondsLeft(uint64_t done, uint64_t total, double speed) {
  if (speed <= 1.0 || total <= done) return total <= done && total ? 0.0 : -1.0;
  return double(total - done) / speed;
}

}  // namespace kk::content
