// This file is UTF-8.
// Beta 7 Extras: the Weta Artbook (owner, 17 Oct 2026; DECISIONS "Beta 7 Extras: Artbook + screensaver", ROADMAP
// Beta 7). The pictures are never shipped: the launcher downloads the PS2 USA bonus disc from archive.org, turns
// its raw MODE2/2352 sectors into the 2048-byte ISO user data, reads the ISO 9660 directory itself (Joliet names,
// no 7-Zip or other programs) and keeps only the six paintings and the readme's text.
//
// Everything here is pure (no files, no OS): raw sector user data, the ISO 9660 / Joliet directory reader over a
// sector-read callback, the readme's text from its Word 97 .doc (compound file + piece table), titles from file
// names, the XDG pictures folder and numbered file names for Save to PC. Tested by tests/artbook_tests.cpp.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kk::artbook {

// ------------------------------------------------------------------ the disc ---
inline constexpr const char* kDiscBase =
    "https://archive.org/download/peterjacksonskingkongtheofficialgameofthemovieusabonusdisc/";
inline constexpr const char* kDiscFile = "Peter Jackson's King Kong - The Official Game of the Movie (USA) (Bonus Disc).bin";
// kDiscBase + UrlEncodePath(kDiscFile) (checked by the tests).
inline constexpr const char* kDiscUrl =
    "https://archive.org/download/peterjacksonskingkongtheofficialgameofthemovieusabonusdisc/"
    "Peter%20Jackson%27s%20King%20Kong%20-%20The%20Official%20Game%20of%20the%20Movie%20%28USA%29%20%28Bonus%20Disc%29.bin";
inline constexpr uint64_t kDiscSize = 34802544;
inline constexpr const char* kDiscSha256 = "cc051bd243281ac1948c41352dd584590e61e4e17e34581f878fb1c0d659af7f";
// What it is turned into while installing: the ISO (30,304,256 bytes) and the pictures (about 17 MB).
inline constexpr uint64_t kInstallBytes = 30304256ull + 17100000ull;

// Where the pictures and the readme are on the disc (Joliet names).
inline constexpr const char* kArtFolder[] = {"BonusMaterials", "Collector's Art", "Weta Collector's Artwork"};
inline constexpr const char* kReadmeFolder[] = {"BonusMaterials", "Collector's Art", "Readme"};
inline constexpr int kPaintingCount = 6;
inline constexpr const char* kReadmeFile = "readme.txt";  // the readme's text, UTF-8, in the install folder
inline constexpr uint64_t kMaxPictureBytes = 64ull << 20;
inline constexpr uint64_t kMaxDocBytes = 4ull << 20;

// Percent-encodes everything but RFC 3986's unreserved characters and '/'.
inline std::string UrlEncodePath(std::string_view s) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
        c == '_' || c == '~' || c == '/') {
      out += char(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

// ------------------------------------------------------------- raw sectors ---
inline constexpr size_t kRawSector = 2352;
inline constexpr size_t kIsoSector = 2048;

enum class SectorError { kNone, kSync, kMode, kForm2 };

// The 2048 bytes of user data of one raw CD sector: Mode 1 at offset 16, Mode 2 Form 1 (XA) at offset 24 (after
// the sync, header and the 8-byte subheader). A Mode 2 Form 2 sector holds 2324 bytes of something else (audio,
// video) and is no ISO data.
inline SectorError UserData(const uint8_t* raw, const uint8_t** data) {
  static const uint8_t kSync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
  if (std::memcmp(raw, kSync, 12) != 0) return SectorError::kSync;
  if (raw[15] == 1) {
    *data = raw + 16;
    return SectorError::kNone;
  }
  if (raw[15] != 2) return SectorError::kMode;
  if (raw[18] & 0x20) return SectorError::kForm2;  // subheader submode, Form 2 bit
  *data = raw + 24;
  return SectorError::kNone;
}

// Converts whole raw sectors (`count` * 2352 bytes) to user data (`count` * 2048 bytes). False at the first sector
// that is not Mode 1 / Mode 2 Form 1 (`bad` = its index in this block).
inline bool ConvertSectors(const uint8_t* raw, size_t count, uint8_t* out, size_t* bad = nullptr,
                           SectorError* error = nullptr) {
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* data = nullptr;
    const SectorError e = UserData(raw + i * kRawSector, &data);
    if (e != SectorError::kNone) {
      if (bad) *bad = i;
      if (error) *error = e;
      return false;
    }
    std::memcpy(out + i * kIsoSector, data, kIsoSector);
  }
  return true;
}

// ------------------------------------------------------------------ text ---
inline void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

// UTF-16 code units (big or little endian already resolved) to UTF-8; lone surrogates become U+FFFD.
inline std::string Utf16ToUtf8(const std::vector<uint16_t>& units) {
  std::string out;
  for (size_t i = 0; i < units.size(); ++i) {
    uint32_t c = units[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < units.size() && units[i + 1] >= 0xDC00 && units[i + 1] < 0xE000) {
      c = 0x10000 + ((c - 0xD800) << 10) + (units[i + 1] - 0xDC00);
      ++i;
    } else if (c >= 0xD800 && c < 0xE000) {
      c = 0xFFFD;
    }
    AppendUtf8(out, c);
  }
  return out;
}

inline uint32_t Cp1252(uint8_t b) {
  static const uint16_t k80[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                   0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
                                   0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                   0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
  return b >= 0x80 && b < 0xA0 ? k80[b - 0x80] : b;
}

// --------------------------------------------------------------- ISO 9660 ---
struct IsoEntry {
  std::string name;  // UTF-8, without the ";1" version
  bool directory = false;
  uint32_t lba = 0;   // first sector (after any extended attribute record)
  uint32_t size = 0;  // bytes
};

// Reads `count` 2048-byte sectors from `lba` into `out`; false when they are not there.
using SectorReader = std::function<bool(uint32_t lba, uint32_t count, uint8_t* out)>;

inline uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

// A name that is safe as one file name in a folder (no separators, no "." / "..", no control characters).
inline bool SafeName(const std::string& name) {
  if (name.empty() || name == "." || name == ".." || name.size() > 255) return false;
  for (unsigned char c : name)
    if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
        c == '|')
      return false;
  return name.back() != '.' && name.back() != ' ';
}

inline bool SameNameAscii(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

class IsoReader {
 public:
  explicit IsoReader(SectorReader read) : read_(std::move(read)) {}

  // Reads the volume descriptors from sector 16: the Joliet one (long names) when there is one, else the primary
  // volume's (8.3 names). False (and `why`) when it is no ISO 9660 volume.
  bool Open(std::string* why = nullptr) {
    std::vector<uint8_t> s(kIsoSector);
    bool primary = false;
    for (uint32_t lba = 16; lba < 16 + 64; ++lba) {
      if (!read_(lba, 1, s.data())) return Fail(why, "cannot read the volume descriptors");
      if (std::memcmp(s.data() + 1, "CD001", 5) != 0) return Fail(why, "not an ISO 9660 volume");
      const uint8_t type = s[0];
      if (type == 255) break;
      if (type != 1 && type != 2) continue;
      if (Le16(s.data() + 128) != kIsoSector) return Fail(why, "unusual logical block size");
      const bool joliet = type == 2 && s[88] == '%' && s[89] == '/' && (s[90] == '@' || s[90] == 'C' || s[90] == 'E');
      if (type == 2 && !joliet) continue;
      if (type == 1 && primary) continue;
      IsoEntry root;
      if (!Record(s.data() + 156, 34, root, joliet, /*root=*/true)) return Fail(why, "bad root directory record");
      volume_blocks_ = Le32(s.data() + 80);
      if (type == 1) {
        primary = true;
        root_ = root;
        joliet_ = false;
      } else {
        root_ = root;
        joliet_ = true;
        break;  // Joliet wins
      }
    }
    if (!primary && !joliet_) return Fail(why, "no primary volume descriptor");
    open_ = true;
    return true;
  }
  bool joliet() const { return joliet_; }
  const IsoEntry& root() const { return root_; }

  // The entries of a directory ("." and ".." left out). False when it cannot be read or is malformed.
  bool List(const IsoEntry& dir, std::vector<IsoEntry>& out, std::string* why = nullptr) const {
    out.clear();
    if (!open_ || !dir.directory) return Fail(why, "not a directory");
    if (dir.size == 0 || dir.size > (1u << 20)) return Fail(why, "unusual directory size");
    const uint32_t sectors = (dir.size + kIsoSector - 1) / kIsoSector;
    if (volume_blocks_ && uint64_t(dir.lba) + sectors > volume_blocks_) return Fail(why, "directory past the volume");
    std::vector<uint8_t> d(size_t(sectors) * kIsoSector);
    if (!read_(dir.lba, sectors, d.data())) return Fail(why, "cannot read a directory");
    size_t off = 0;
    while (off < dir.size) {
      const uint8_t len = d[off];
      if (len == 0) {  // the rest of this sector is empty: records never cross a sector
        off = (off / kIsoSector + 1) * kIsoSector;
        continue;
      }
      if (len < 34 || (off % kIsoSector) + len > kIsoSector) return Fail(why, "bad directory record");
      const uint8_t name_len = d[off + 32];
      const bool self = name_len == 1 && (d[off + 33] == 0 || d[off + 33] == 1);
      if (!self) {
        IsoEntry e;
        if (!Record(d.data() + off, len, e, joliet_, false)) return Fail(why, "bad directory record");
        if (volume_blocks_ && !e.directory && e.size &&
            uint64_t(e.lba) + (uint64_t(e.size) + kIsoSector - 1) / kIsoSector > volume_blocks_)
          return Fail(why, "file past the volume");
        if (e.name.empty()) return Fail(why, "unnamed entry");
        out.push_back(std::move(e));
      }
      off += len;
    }
    return true;
  }

  // Follows folder names from the root (ASCII letters compared without case). Empty when one is missing.
  std::optional<IsoEntry> Find(const std::vector<std::string>& path) const {
    IsoEntry at = root_;
    std::vector<IsoEntry> entries;
    for (const std::string& part : path) {
      if (!List(at, entries)) return std::nullopt;
      const auto it = std::find_if(entries.begin(), entries.end(),
                                   [&](const IsoEntry& e) { return SameNameAscii(e.name, part); });
      if (it == entries.end()) return std::nullopt;
      at = *it;
    }
    return at;
  }

  // A file's bytes, `chunk` sectors at a time, passed to `sink` (false stops: returns false).
  bool Read(const IsoEntry& file, const std::function<bool(const uint8_t*, size_t)>& sink, uint32_t chunk = 64) const {
    if (file.directory) return false;
    std::vector<uint8_t> buf(size_t(chunk) * kIsoSector);
    uint64_t left = file.size;
    uint32_t lba = file.lba;
    while (left > 0) {
      const uint32_t n = uint32_t(std::min<uint64_t>(chunk, (left + kIsoSector - 1) / kIsoSector));
      if (!read_(lba, n, buf.data())) return false;
      const size_t bytes = size_t(std::min<uint64_t>(left, uint64_t(n) * kIsoSector));
      if (!sink(buf.data(), bytes)) return false;
      left -= bytes;
      lba += n;
    }
    return true;
  }

 private:
  static bool Fail(std::string* why, const char* text) {
    if (why) *why = text;
    return false;
  }
  // One directory record (ECMA-119 9.1).
  static bool Record(const uint8_t* r, size_t len, IsoEntry& e, bool joliet, bool root) {
    if (len < 34 || r[0] < 34 || r[0] > len) return false;
    const uint8_t name_len = r[32];
    if (33u + name_len > r[0]) return false;
    if (r[25] & 0x80) return false;  // a file in several extents: not used on this disc
    e.directory = (r[25] & 0x02) != 0;
    e.lba = Le32(r + 2) + r[1];
    e.size = Le32(r + 10);
    if (root) return e.directory;
    const uint8_t* n = r + 33;
    if (joliet) {
      std::vector<uint16_t> units;
      for (size_t i = 0; i + 1 < name_len; i += 2) units.push_back(uint16_t(n[i] << 8 | n[i + 1]));
      e.name = Utf16ToUtf8(units);
    } else {
      for (size_t i = 0; i < name_len; ++i) AppendUtf8(e.name, Cp1252(n[i]));
    }
    if (const size_t semi = e.name.rfind(';'); semi != std::string::npos) e.name.resize(semi);
    if (!joliet && !e.directory && !e.name.empty() && e.name.back() == '.') e.name.pop_back();  // "README." -> "README"
    return true;
  }

  SectorReader read_;
  IsoEntry root_;
  uint32_t volume_blocks_ = 0;
  bool joliet_ = false;
  bool open_ = false;
};

// ------------------------------------------------- the readme (Word 97 .doc) ---
namespace detail {
struct Cfb {
  const std::vector<uint8_t>& d;
  explicit Cfb(const std::vector<uint8_t>& bytes) : d(bytes) {}
  uint32_t shift = 9, mini_shift = 6, cutoff = 4096;
  std::vector<uint32_t> fat, minifat;
  std::vector<uint8_t> ministream;
  struct Dir {
    std::string name;
    uint8_t type;
    uint32_t start;
    uint64_t size;
  };
  std::vector<Dir> dirs;

  bool SectorOk(uint32_t n) const { return (uint64_t(n) + 2) << shift <= d.size(); }
  const uint8_t* Sector(uint32_t n) const { return d.data() + ((uint64_t(n) + 1) << shift); }
  // Follows a chain in `table`; false on a loop, a bad index or more than `limit` entries.
  static bool Chain(const std::vector<uint32_t>& table, uint32_t start, size_t limit, std::vector<uint32_t>& out) {
    out.clear();
    for (uint32_t s = start; s < 0xFFFFFFFA; s = table[s]) {
      if (s >= table.size() || out.size() >= limit) return false;
      out.push_back(s);
    }
    return true;
  }
  bool ReadChain(uint32_t start, std::vector<uint8_t>& out) const {
    std::vector<uint32_t> chain;
    if (!Chain(fat, start, d.size() >> shift, chain)) return false;
    out.clear();
    for (uint32_t s : chain) {
      if (!SectorOk(s)) return false;
      out.insert(out.end(), Sector(s), Sector(s) + (size_t(1) << shift));
    }
    return true;
  }
  bool Open() {
    static const uint8_t kMagic[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    if (d.size() < 512 || std::memcmp(d.data(), kMagic, 8) != 0) return false;
    shift = Le16(d.data() + 30);
    mini_shift = Le16(d.data() + 32);
    if ((shift != 9 && shift != 12) || mini_shift != 6) return false;
    const uint32_t nfat = Le32(d.data() + 44), dir_start = Le32(d.data() + 48);
    cutoff = Le32(d.data() + 56);
    const uint32_t minifat_start = Le32(d.data() + 60);
    uint32_t difat_next = Le32(d.data() + 68), ndifat = Le32(d.data() + 72);
    std::vector<uint32_t> fat_sectors;
    for (int i = 0; i < 109 && fat_sectors.size() < nfat; ++i) fat_sectors.push_back(Le32(d.data() + 76 + 4 * i));
    const uint32_t per = (1u << shift) / 4;
    for (uint32_t k = 0; k < ndifat && fat_sectors.size() < nfat && k < 1024; ++k) {
      if (!SectorOk(difat_next)) return false;
      const uint8_t* p = Sector(difat_next);
      for (uint32_t i = 0; i + 1 < per && fat_sectors.size() < nfat; ++i) fat_sectors.push_back(Le32(p + 4 * i));
      difat_next = Le32(p + 4 * (per - 1));
    }
    if (fat_sectors.size() != nfat || nfat == 0 || nfat > 4096) return false;
    for (uint32_t s : fat_sectors) {
      if (!SectorOk(s)) return false;
      for (uint32_t i = 0; i < per; ++i) fat.push_back(Le32(Sector(s) + 4 * i));
    }
    std::vector<uint8_t> dir;
    if (!ReadChain(dir_start, dir)) return false;
    for (size_t off = 0; off + 128 <= dir.size(); off += 128) {
      const uint8_t* e = dir.data() + off;
      const uint16_t name_bytes = std::min<uint16_t>(Le16(e + 64), 64);
      std::vector<uint16_t> units;
      for (size_t i = 0; i + 1 < name_bytes; i += 2) {
        const uint16_t u = Le16(e + i);
        if (u == 0) break;
        units.push_back(u);
      }
      dirs.push_back({Utf16ToUtf8(units), e[66], Le32(e + 116), uint64_t(Le32(e + 120)) | (shift == 12 ? uint64_t(Le32(e + 124)) << 32 : 0)});
    }
    if (dirs.empty() || dirs[0].type != 5) return false;
    if (!ReadChain(dirs[0].start, ministream) && dirs[0].size) return false;
    std::vector<uint8_t> mf;
    if (minifat_start < 0xFFFFFFFA && ReadChain(minifat_start, mf))
      for (size_t i = 0; i + 4 <= mf.size(); i += 4) minifat.push_back(Le32(mf.data() + i));
    return true;
  }
  bool Stream(std::string_view name, std::vector<uint8_t>& out) const {
    for (const Dir& e : dirs) {
      if (e.type != 2 || e.name != name) continue;
      if (e.size > d.size()) return false;
      if (e.size < cutoff) {
        std::vector<uint32_t> chain;
        if (!Chain(minifat, e.start, ministream.size() >> mini_shift, chain)) return false;
        out.clear();
        for (uint32_t s : chain) {
          const size_t at = size_t(s) << mini_shift;
          if (at + (size_t(1) << mini_shift) > ministream.size()) return false;
          out.insert(out.end(), ministream.begin() + at, ministream.begin() + at + (size_t(1) << mini_shift));
        }
      } else if (!ReadChain(e.start, out)) {
        return false;
      }
      if (out.size() < e.size) return false;
      out.resize(size_t(e.size));
      return true;
    }
    return false;
  }
};
}  // namespace detail

// The main text of a Word 97-2003 document as UTF-8 paragraphs ('\n' between them), or empty when it is not one
// this reader understands. Field codes are left out (their results kept), dashes become '-', other control
// characters are dropped.
inline std::string WordDocText(const std::vector<uint8_t>& doc) {
  if (doc.size() > kMaxDocBytes) return {};
  detail::Cfb cfb(doc);
  if (!cfb.Open()) return {};
  std::vector<uint8_t> wd, table;
  if (!cfb.Stream("WordDocument", wd) || wd.size() < 64 || Le16(wd.data()) != 0xA5EC) return {};
  const uint16_t flags = Le16(wd.data() + 10);
  if (flags & 0x0100) return {};  // encrypted
  if (!cfb.Stream((flags & 0x0200) ? "1Table" : "0Table", table)) return {};
  // FIB: csw, rgW, cslw, rgLw (ccpText = its 4th), cbRgFcLcb, rgFcLcb (fcClx / lcbClx = pair 33).
  size_t at = 32;
  const uint16_t csw = Le16(wd.data() + at);
  at += 2 + size_t(csw) * 2;
  if (at + 2 > wd.size()) return {};
  const uint16_t cslw = Le16(wd.data() + at);
  if (cslw < 4 || at + 2 + size_t(cslw) * 4 + 2 > wd.size()) return {};
  const uint32_t ccp_text = Le32(wd.data() + at + 2 + 3 * 4);
  at += 2 + size_t(cslw) * 4;
  const uint16_t pairs = Le16(wd.data() + at);
  if (pairs < 34 || at + 2 + size_t(pairs) * 8 > wd.size()) return {};
  const uint32_t fc_clx = Le32(wd.data() + at + 2 + 33 * 8), lcb_clx = Le32(wd.data() + at + 2 + 33 * 8 + 4);
  if (uint64_t(fc_clx) + lcb_clx > table.size() || lcb_clx < 5) return {};
  const uint8_t* clx = table.data() + fc_clx;
  size_t i = 0;
  while (i < lcb_clx && clx[i] == 1) {  // Prc: property modifiers, skipped
    if (i + 3 > lcb_clx) return {};
    i += 3 + Le16(clx + i + 1);
  }
  if (i + 5 > lcb_clx || clx[i] != 2) return {};
  const uint32_t lcb = Le32(clx + i + 1);
  const uint8_t* plc = clx + i + 5;
  if (i + 5 + uint64_t(lcb) > lcb_clx || lcb < 16 || (lcb - 4) % 12) return {};
  const uint32_t n = (lcb - 4) / 12;
  std::vector<uint32_t> units;  // code points
  for (uint32_t k = 0; k < n && units.size() < ccp_text; ++k) {
    const uint32_t cp0 = Le32(plc + 4 * k), cp1 = Le32(plc + 4 * (k + 1));
    if (cp1 < cp0) return {};
    uint32_t fc = Le32(plc + 4 * (n + 1) + 8 * k + 2);
    const bool compressed = (fc & 0x40000000) != 0;
    fc &= 0x3FFFFFFF;
    const uint32_t count = std::min(cp1 - cp0, ccp_text - uint32_t(units.size()));
    if (compressed) {
      const uint64_t from = fc / 2;
      if (from + count > wd.size()) return {};
      for (uint32_t c = 0; c < count; ++c) units.push_back(Cp1252(wd[size_t(from + c)]));
    } else {
      if (uint64_t(fc) + 2ull * count > wd.size()) return {};
      std::vector<uint16_t> u16;
      for (uint32_t c = 0; c < count; ++c) u16.push_back(Le16(wd.data() + fc + 2 * c));
      for (size_t c = 0; c < u16.size(); ++c) {
        uint32_t v = u16[c];
        if (v >= 0xD800 && v < 0xDC00 && c + 1 < u16.size()) v = 0x10000 + ((v - 0xD800) << 10) + (u16[++c] - 0xDC00);
        units.push_back(v);
      }
    }
  }
  std::string out;
  int field = 0;          // nesting of field codes (0x13 begin, 0x14 separator, 0x15 end)
  bool instruction = false;
  for (uint32_t c : units) {
    if (c == 0x13) { ++field; instruction = true; continue; }
    if (c == 0x14) { instruction = false; continue; }
    if (c == 0x15) { if (field) --field; instruction = false; continue; }
    if (instruction && field) continue;
    if (c == 0x0D || c == 0x0B || c == 0x0C) c = '\n';
    else if (c == 0x07 || c == 0x09) c = ' ';
    else if (c == 0x1E || c == 0x2010 || c == 0x2011 || c == 0x2012 || c == 0x2013 || c == 0x2014 || c == 0x2015) c = '-';
    else if (c == 0xA0) c = ' ';
    else if (c == 0x1F || c == 0xFFFD || (c < 0x20 && c != '\n')) continue;
    AppendUtf8(out, c);
  }
  // Tidy: no spaces at line ends, at most one empty line, none at the ends.
  std::string tidy;
  size_t start = 0;
  int blank = 0;
  while (start <= out.size()) {
    size_t end = out.find('\n', start);
    if (end == std::string::npos) end = out.size();
    std::string line = out.substr(start, end - start);
    while (!line.empty() && line.back() == ' ') line.pop_back();
    while (!line.empty() && line.front() == ' ') line.erase(line.begin());
    if (line.empty()) {
      ++blank;
    } else {
      if (!tidy.empty()) tidy += blank ? "\n\n" : "\n";
      tidy += line;
      blank = 0;
    }
    start = end + 1;
  }
  return tidy;
}

// ------------------------------------------------------------ small helpers ---
// "KONG_Courtyard.jpg" -> "Courtyard" (the extension and a "KONG_" prefix off, underscores as spaces).
inline std::string TitleFromFile(std::string_view file) {
  std::string s(file);
  if (const size_t dot = s.rfind('.'); dot != std::string::npos && dot > 0) s.resize(dot);
  if (s.size() > 5 && SameNameAscii(std::string_view(s).substr(0, 5), "kong_")) s.erase(0, 5);
  std::replace(s.begin(), s.end(), '_', ' ');
  return s;
}

inline bool HasExtension(std::string_view name, std::string_view ext) {
  return name.size() > ext.size() && SameNameAscii(name.substr(name.size() - ext.size()), ext);
}

// "KONG_Rex.jpg", 2 -> "KONG_Rex (2).jpg".
inline std::string NumberedName(std::string_view file, int n) {
  if (n <= 1) return std::string(file);
  const size_t dot = file.rfind('.');
  const std::string stem(file.substr(0, dot == std::string_view::npos || dot == 0 ? file.size() : dot));
  const std::string ext(dot == std::string_view::npos || dot == 0 ? std::string_view() : file.substr(dot));
  return stem + " (" + std::to_string(n) + ")" + ext;
}

// The pictures folder from the text of $XDG_CONFIG_HOME/user-dirs.dirs (XDG_PICTURES_DIR="$HOME/Pictures"), or
// <home>/Pictures when it is not set there (or set to the home folder itself, which the spec means as "none").
inline std::string XdgPicturesDir(std::string_view user_dirs, const std::string& home) {
  std::string found;
  size_t start = 0;
  while (start < user_dirs.size()) {
    size_t end = user_dirs.find('\n', start);
    if (end == std::string_view::npos) end = user_dirs.size();
    std::string_view line = user_dirs.substr(start, end - start);
    start = end + 1;
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
    constexpr std::string_view kKey = "XDG_PICTURES_DIR=";
    if (line.substr(0, kKey.size()) != kKey) continue;
    std::string_view v = line.substr(kKey.size());
    if (v.size() < 2 || v.front() != '"' || v.back() != '"') continue;
    v = v.substr(1, v.size() - 2);
    std::string value;
    if (v.substr(0, 5) == "$HOME") {
      value = home + std::string(v.substr(5));
    } else if (!v.empty() && v.front() == '/') {
      value = std::string(v);
    } else {
      continue;  // relative paths are not allowed by the spec
    }
    // Unescape \" \\ \$ (the format's only escapes).
    std::string plain;
    for (size_t i = 0; i < value.size(); ++i) {
      if (value[i] == '\\' && i + 1 < value.size()) ++i;
      plain += value[i];
    }
    while (plain.size() > 1 && plain.back() == '/') plain.pop_back();
    std::string h = home;
    while (h.size() > 1 && h.back() == '/') h.pop_back();
    found = plain == h ? std::string() : plain;
  }
  if (!found.empty()) return found;
  std::string h = home;
  while (h.size() > 1 && h.back() == '/') h.pop_back();
  return h + "/Pictures";
}

// The width and height in a JPEG's frame header (SOF0-SOF15 but DHT/JPG/DAC), false when the bytes are not a
// JPEG with one before the image data.
inline bool JpegSize(const uint8_t* p, size_t n, int& width, int& height) {
  if (n < 4 || p[0] != 0xFF || p[1] != 0xD8) return false;
  size_t i = 2;
  while (i + 4 <= n) {
    if (p[i] != 0xFF) return false;
    const uint8_t m = p[i + 1];
    if (m == 0xFF) { ++i; continue; }
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
    if (m == 0xD9 || m == 0xDA) return false;  // end, or image data before any frame header
    const size_t len = size_t(p[i + 2]) << 8 | p[i + 3];
    if (len < 2 || i + 2 + len > n) return false;
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      if (len < 7) return false;
      height = int(p[i + 5]) << 8 | p[i + 6];
      width = int(p[i + 7]) << 8 | p[i + 8];
      return width > 0 && height > 0;
    }
    i += 2 + len;
  }
  return false;
}

}  // namespace kk::artbook
