// Beta 7 Extras > Artbook without the network or the real disc: the download address, raw CD sectors to ISO user
// data, our ISO 9660 / Joliet reader on a synthetic disc built here (and malformed ones), the readme's text from a
// synthetic Word 97 file, titles and file names, the XDG pictures folder, the streaming conversion and the
// extraction on files, Save to PC into a test Pictures folder (KK_DEV_ARTBOOK_PICTURES), the automatic-download
// state, and the content service downloading and installing a synthetic disc from a fake server.
// Usage: kk-artbook-tests <empty work folder>
#include "artbook_files.h"
#include "artbook_logic.h"
#include "content.h"
#include "content_logic.h"
#include "workshop_net.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>

#include "workshop_test_pictures.inc"

#if !defined(_WIN32)
// workshop_net.cpp's Linux HTTPS GET runs curl through platform.cpp; these tests never fetch anything.
namespace kk {
bool RunCapture(const std::vector<std::string>&, std::vector<uint8_t>&, std::atomic<uint64_t>*) { return false; }
}  // namespace kk
#endif

namespace fs = std::filesystem;
using namespace kk::artbook;

static int checks;
#define CHECK(x)                                                 \
  do {                                                           \
    ++checks;                                                    \
    if (!(x)) {                                                  \
      std::cerr << "FAIL line " << __LINE__ << ": " #x "\n";     \
      std::exit(1);                                              \
    }                                                            \
  } while (0)

static void WriteFile(const fs::path& p, const std::string& data) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary).write(data.data(), std::streamsize(data.size()));
}
static std::string ReadFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}
static std::string Jpeg() { return std::string(reinterpret_cast<const char*>(kTestJpeg), sizeof(kTestJpeg)); }
static std::string Sha(const std::string& s) {
  kk::content::Sha256 h;
  h.Update(s.data(), s.size());
  return h.HexDigest();
}
static void Put16(std::string& o, size_t at, uint32_t v) { o[at] = char(v & 255); o[at + 1] = char((v >> 8) & 255); }
static void Put32(std::string& o, size_t at, uint32_t v) { Put16(o, at, v & 0xFFFF); Put16(o, at + 2, v >> 16); }
static void PutBoth32(std::string& o, size_t at, uint32_t v) {  // ISO 9660 both-endian
  Put32(o, at, v);
  o[at + 4] = char(v >> 24); o[at + 5] = char(v >> 16); o[at + 6] = char(v >> 8); o[at + 7] = char(v);
}

// ------------------------------------------------------------ synthetic ISO ---
struct Node {
  std::string name, short_name;  // Joliet name, primary volume (8.3) name
  bool dir = false;
  std::string data;
  std::vector<Node> kids;
  uint32_t lba = 0, plba = 0, jlba = 0;
};
static Node Dir(std::string name, std::string short_name, std::vector<Node> kids) {
  Node n{std::move(name), std::move(short_name), true, {}, std::move(kids)};
  return n;
}
static Node FileNode(std::string name, std::string short_name, std::string data) {
  return Node{std::move(name), std::move(short_name), false, std::move(data), {}};
}

struct IsoOptions {
  bool joliet = true;
  bool cross_sector_record = false;  // a directory record that runs past its sector
};

static std::string Record(uint32_t lba, uint32_t size, bool dir, const std::string& name) {
  size_t len = 33 + name.size();
  if (len & 1) ++len;
  std::string r(len, '\0');
  r[0] = char(len);
  PutBoth32(r, 2, lba);
  PutBoth32(r, 10, size);
  r[25] = dir ? 2 : 0;
  r[28] = 1; r[31] = 1;
  r[32] = char(name.size());
  std::memcpy(&r[33], name.data(), name.size());
  return r;
}
static std::string JolietName(const std::string& s) {
  std::string o;
  for (unsigned char c : s) { o += '\0'; o += char(c); }
  return o;
}

static void Assign(Node& n, uint32_t& next) {
  if (n.dir) {
    n.plba = next++;
    n.jlba = next++;
    for (Node& k : n.kids) Assign(k, next);
  } else {
    n.lba = next;
    next += uint32_t((n.data.size() + 2047) / 2048);
  }
}
static void WriteDirs(std::string& iso, const Node& n, const Node& parent, const IsoOptions& o) {
  if (n.dir) {
    for (int joliet = 0; joliet < (o.joliet ? 2 : 1); ++joliet) {
      std::string d;
      const uint32_t self = joliet ? n.jlba : n.plba, up = joliet ? parent.jlba : parent.plba;
      d += Record(self, 2048, true, std::string(1, '\0'));
      d += Record(up, 2048, true, std::string(1, '\1'));
      for (const Node& k : n.kids) {
        const std::string nm = joliet ? JolietName(k.name + (k.dir ? "" : ";1")) : k.short_name + (k.dir ? "" : ";1");
        d += Record(k.dir ? (joliet ? k.jlba : k.plba) : k.lba, k.dir ? 2048 : uint32_t(k.data.size()), k.dir, nm);
      }
      if (o.cross_sector_record) {  // valid empty records up to near the sector's end, then one running past it
        for (int k = 0; d.size() < 2010; ++k) d += Record(0, 0, false, "PAD" + std::to_string(1000 + k) + ".X;1");
        d += Record(0, 0, false, "LONGNAMEOVER.JPG;1");
      }
      d.resize(std::max<size_t>(d.size(), 2048), '\0');
      std::memcpy(&iso[size_t(self) * 2048], d.data(), 2048);
    }
    for (const Node& k : n.kids) WriteDirs(iso, k, n, o);
  } else {
    std::memcpy(&iso[size_t(n.lba) * 2048], n.data.data(), n.data.size());
  }
}
static std::string MakeIso(Node root, const IsoOptions& o = {}) {
  uint32_t next = 19;
  Assign(root, next);
  std::string iso(size_t(next) * 2048, '\0');
  for (int i = 0; i < 3; ++i) {
    const size_t at = size_t(16 + i) * 2048;
    const bool term = i == 2 || (i == 1 && !o.joliet);
    iso[at] = char(term ? 255 : i + 1);
    std::memcpy(&iso[at + 1], "CD001", 5);
    iso[at + 6] = 1;
    if (term) {
      if (i == 1) iso[size_t(18) * 2048] = 0;  // no third descriptor
      continue;
    }
    PutBoth32(iso, at + 80, next);
    Put16(iso, at + 128, 2048);
    iso[at + 130] = 8; iso[at + 131] = 0;
    if (i == 1) std::memcpy(&iso[at + 88], "%/E", 3);
    const std::string r = Record(i == 1 ? root.jlba : root.plba, 2048, true, std::string(1, '\0'));
    std::memcpy(&iso[at + 156], r.data(), 34);
  }
  WriteDirs(iso, root, root, o);
  return iso;
}

// Raw MODE2/2352 Form 1 sectors (mode 1 when `mode1`); ECC/EDC left zero (not checked).
static std::string MakeRaw(const std::string& iso, bool mode1 = false, size_t form2_at = SIZE_MAX) {
  std::string raw;
  for (size_t i = 0; i * 2048 < iso.size(); ++i) {
    std::string s(2352, '\0');
    for (int k = 1; k < 11; ++k) s[k] = char(0xFF);
    s[15] = char(mode1 ? 1 : 2);
    if (!mode1 && i == form2_at) s[18] = s[22] = 0x20;
    std::memcpy(&s[mode1 ? 16 : 24], iso.data() + i * 2048, 2048);
    raw += s;
  }
  return raw;
}

// ----------------------------------------------------------- synthetic .doc ---
// A Word 97 file: compound file (512-byte sectors; WordDocument and 1Table 4096 bytes each, so no mini stream),
// a FIB with ccpText and fcClx, and a piece table with one 8-bit (cp1252) piece and one UTF-16 piece.
static std::string MakeDoc(const std::string& cp1252, const std::u16string& utf16) {
  std::string wd(4096, '\0'), table(4096, '\0');
  Put16(wd, 0, 0xA5EC);
  Put16(wd, 2, 0xC1);
  Put16(wd, 10, 0x0200);  // fWhichTblStm: 1Table
  Put16(wd, 32, 14);      // csw
  Put16(wd, 62, 22);      // cslw
  const uint32_t ccp = uint32_t(cp1252.size() + utf16.size());
  Put32(wd, 64 + 3 * 4, ccp);
  Put16(wd, 152, 93);     // cbRgFcLcb
  const size_t text8 = 1024, text16 = 1536;
  std::memcpy(&wd[text8], cp1252.data(), cp1252.size());
  for (size_t i = 0; i < utf16.size(); ++i) Put16(wd, text16 + 2 * i, utf16[i]);
  // CLX in 1Table: Pcdt with two pieces.
  const uint32_t n = 2, lcb = 4 * (n + 1) + 8 * n;
  table[0] = 2;
  Put32(table, 1, lcb);
  Put32(table, 5, 0);
  Put32(table, 9, uint32_t(cp1252.size()));
  Put32(table, 13, ccp);
  Put32(table, 17 + 2, uint32_t(text8 * 2) | 0x40000000);  // compressed: byte offset * 2
  Put32(table, 25 + 2, uint32_t(text16));
  Put32(wd, 154 + 33 * 8, 0);        // fcClx
  Put32(wd, 154 + 33 * 8 + 4, 5 + lcb);  // lcbClx
  // Compound file: header, FAT (sector 0), directory (1), WordDocument (2-9), 1Table (10-17).
  std::string f(512 * 19, '\0');
  const uint8_t magic[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
  std::memcpy(&f[0], magic, 8);
  Put16(f, 24, 0x3E); Put16(f, 26, 3); Put16(f, 28, 0xFFFE); Put16(f, 30, 9); Put16(f, 32, 6);
  Put32(f, 44, 1); Put32(f, 48, 1); Put32(f, 56, 4096); Put32(f, 60, 0xFFFFFFFE); Put32(f, 68, 0xFFFFFFFE);
  for (int i = 0; i < 109; ++i) Put32(f, 76 + 4 * i, i == 0 ? 0 : 0xFFFFFFFF);
  std::string fat(512, '\xFF');
  Put32(fat, 0, 0xFFFFFFFD);
  Put32(fat, 4, 0xFFFFFFFE);
  for (uint32_t s = 2; s < 18; ++s) Put32(fat, 4 * s, s == 9 || s == 17 ? 0xFFFFFFFE : s + 1);
  std::memcpy(&f[512], fat.data(), 512);
  std::string dir(512, '\0');
  auto entry = [&](int i, const std::string& name, uint8_t type, uint32_t start, uint32_t size) {
    const size_t at = size_t(i) * 128;
    for (size_t k = 0; k < name.size(); ++k) Put16(dir, at + 2 * k, uint8_t(name[k]));
    Put16(dir, at + 64, uint32_t(name.size() + 1) * 2);
    dir[at + 66] = char(type);
    Put32(dir, at + 68, 0xFFFFFFFF); Put32(dir, at + 72, 0xFFFFFFFF); Put32(dir, at + 76, 0xFFFFFFFF);
    Put32(dir, at + 116, start);
    Put32(dir, at + 120, size);
  };
  entry(0, "Root Entry", 5, 0xFFFFFFFE, 0);
  entry(1, "WordDocument", 2, 2, 4096);
  entry(2, "1Table", 2, 10, 4096);
  std::memcpy(&f[512 * 2], dir.data(), 512);
  std::memcpy(&f[512 * 3], wd.data(), 4096);
  std::memcpy(&f[512 * 11], table.data(), 4096);
  return f;
}

static Node DiscTree(const std::vector<std::pair<std::string, std::string>>& pictures, const std::string& doc,
                     bool with_exe = true) {
  std::vector<Node> art;
  int i = 0;
  for (const auto& [name, data] : pictures) art.push_back(FileNode(name, "KONG_" + std::to_string(i++) + ".JPG", data));
  std::vector<Node> collector = {Dir("Readme", "README", {FileNode("Collector'sArtReadme.doc", "COLLECTO.DOC", doc)}),
                                 Dir("Weta Collector's Artwork", "WETACOLL", art)};
  std::vector<Node> bonus = {Dir("Collector's Art", "COLLECTO", collector)};
  if (with_exe)
    bonus.push_back(Dir("Dynamic Screensaver", "DYNAMICS", {FileNode("King_Kong_Install.exe", "KING_KON.EXE", "MZ not run")}));
  return Dir("", "", {Dir("BonusMaterials", "BONUSMAT", bonus)});
}

static SectorReader Reader(const std::string& iso) {
  return [&iso](uint32_t lba, uint32_t count, uint8_t* out) {
    if ((uint64_t(lba) + count) * 2048 > iso.size()) return false;
    std::memcpy(out, iso.data() + size_t(lba) * 2048, size_t(count) * 2048);
    return true;
  };
}

// ------------------------------------------------------------------ tests ---
static void Pure() {
  CHECK(std::string(kDiscBase) + UrlEncodePath(kDiscFile) == kDiscUrl);
  CHECK(UrlEncodePath("a b'c(d)e/f~g_h-i.j") == "a%20b%27c%28d%29e/f~g_h-i.j");
  CHECK(UrlEncodePath("\xC3\xA9") == "%C3%A9");

  // Sectors.
  std::string iso(3 * 2048, '\0');
  for (size_t i = 0; i < iso.size(); ++i) iso[i] = char(i * 7 + i / 2048);
  std::string raw = MakeRaw(iso), out(iso.size(), '\0');
  CHECK(raw.size() == 3 * 2352);
  CHECK(ConvertSectors(reinterpret_cast<const uint8_t*>(raw.data()), 3, reinterpret_cast<uint8_t*>(out.data())));
  CHECK(out == iso);
  std::string raw1 = MakeRaw(iso, true);
  out.assign(iso.size(), '\0');
  CHECK(ConvertSectors(reinterpret_cast<const uint8_t*>(raw1.data()), 3, reinterpret_cast<uint8_t*>(out.data())));
  CHECK(out == iso);
  std::string raw2 = MakeRaw(iso, false, 1);
  size_t bad = 99;
  SectorError e = SectorError::kNone;
  CHECK(!ConvertSectors(reinterpret_cast<const uint8_t*>(raw2.data()), 3, reinterpret_cast<uint8_t*>(out.data()), &bad, &e));
  CHECK(bad == 1 && e == SectorError::kForm2);
  raw2 = raw;
  raw2[2352 * 2 + 5] = 0;  // broken sync
  CHECK(!ConvertSectors(reinterpret_cast<const uint8_t*>(raw2.data()), 3, reinterpret_cast<uint8_t*>(out.data()), &bad, &e));
  CHECK(bad == 2 && e == SectorError::kSync);
  raw2 = raw;
  raw2[15] = 3;
  CHECK(!ConvertSectors(reinterpret_cast<const uint8_t*>(raw2.data()), 1, reinterpret_cast<uint8_t*>(out.data()), &bad, &e));
  CHECK(e == SectorError::kMode);

  // ISO 9660 with Joliet names.
  const std::string doc = MakeDoc("Weta\x96made \x13 HYPERLINK x \x14link\x15 text\xAE\r\r\r", u"Second para—end\r");
  std::string big(5000, 'q');
  big.replace(0, Jpeg().size(), Jpeg());
  const std::string disc = MakeIso(DiscTree({{"KONG_Courtyard.jpg", Jpeg()}, {"KONG_Wall.jpg", big}}, doc));
  {
    IsoReader r(Reader(disc));
    std::string why;
    CHECK(r.Open(&why));
    CHECK(r.joliet());
    const auto art = r.Find({std::begin(kArtFolder), std::end(kArtFolder)});
    CHECK(art && art->directory);
    CHECK(r.Find({"bonusmaterials", "COLLECTOR'S ART", "weta collector's artwork"}).has_value());  // case
    CHECK(!r.Find({"BonusMaterials", "Nope"}).has_value());
    std::vector<IsoEntry> list;
    CHECK(r.List(*art, list));
    CHECK(list.size() == 2 && list[0].name == "KONG_Courtyard.jpg" && list[1].name == "KONG_Wall.jpg");
    CHECK(list[1].size == 5000 && !list[1].directory);
    std::string got;
    CHECK(r.Read(list[1], [&](const uint8_t* p, size_t n) { got.append(reinterpret_cast<const char*>(p), n); return true; }, 1));
    CHECK(got == big);
    int calls = 0;
    CHECK(!r.Read(list[1], [&](const uint8_t*, size_t) { return ++calls < 2; }, 1));  // the sink can stop it
    const auto readme = r.Find({std::begin(kReadmeFolder), std::end(kReadmeFolder)});
    CHECK(readme && r.List(*readme, list) && list.size() == 1 && list[0].name == "Collector'sArtReadme.doc");
  }
  // The primary volume only: 8.3 names, and the long path is not there.
  {
    IsoOptions o;
    o.joliet = false;
    const std::string plain = MakeIso(DiscTree({{"KONG_Rex.jpg", Jpeg()}}, doc), o);
    IsoReader r(Reader(plain));
    CHECK(r.Open() && !r.joliet());
    CHECK(!r.Find({std::begin(kArtFolder), std::end(kArtFolder)}).has_value());
    const auto art = r.Find({"BONUSMAT", "COLLECTO", "WETACOLL"});
    std::vector<IsoEntry> list;
    CHECK(art && r.List(*art, list) && list.size() == 1 && list[0].name == "KONG_0.JPG");
  }
  // Malformed: not ISO, a record that runs past its sector, a file past the end of the volume, a truncated disc.
  {
    std::string junk(40 * 2048, 'x');
    IsoReader r(Reader(junk));
    std::string why;
    CHECK(!r.Open(&why) && !why.empty());
    IsoOptions o;
    o.cross_sector_record = true;
    const std::string crossing = MakeIso(DiscTree({{"KONG_Rex.jpg", Jpeg()}}, doc), o);
    IsoReader r2(Reader(crossing));
    CHECK(r2.Open());
    std::vector<IsoEntry> list;
    CHECK(!r2.List(r2.root(), list, &why));
    std::string past = disc;
    // KONG_Wall.jpg's size: far past the volume.
    IsoReader r3(Reader(past));
    CHECK(r3.Open());
    const auto art = r3.Find({std::begin(kArtFolder), std::end(kArtFolder)});
    const size_t rec = size_t(art->lba) * 2048;
    size_t off = rec;
    for (int i = 0; i < 3; ++i) off += uint8_t(past[off]);  // ".", "..", KONG_Courtyard: the 4th is KONG_Wall
    PutBoth32(past, off + 10, 0x7FFFFFFF);
    IsoReader r4(Reader(past));
    CHECK(r4.Open() && !r4.List(*art, list));
    const std::string cut = disc.substr(0, 17 * 2048);
    IsoReader r5(Reader(cut));
    CHECK(!r5.Open());
  }
  CHECK(SafeName("KONG_Rex.jpg") && SafeName("Collector'sArtReadme.doc"));
  CHECK(!SafeName("..") && !SafeName("a/b") && !SafeName("a\\b") && !SafeName("") && !SafeName("x.") &&
        !SafeName("c:x") && !SafeName(std::string("a\x01")));

  // The readme's text.
  CHECK(WordDocText(std::vector<uint8_t>(doc.begin(), doc.end())) ==
        "Weta-made link text\xC2\xAE\n\nSecond para-end");
  CHECK(WordDocText(std::vector<uint8_t>(100, 0)).empty());
  std::vector<uint8_t> broken(doc.begin(), doc.end());
  broken[512 * 3] = 0;  // the FIB's magic
  CHECK(WordDocText(broken).empty());
  broken.assign(doc.begin(), doc.end());
  broken[512 + 8] = char(0x02);  // a FAT chain that loops: WordDocument's sector 2 -> 2
  broken[512 + 9] = broken[512 + 10] = broken[512 + 11] = 0;
  CHECK(WordDocText(broken).empty());

  // Titles and file names.
  CHECK(TitleFromFile("KONG_Courtyard.jpg") == "Courtyard");
  CHECK(TitleFromFile("kong_Swinging.JPG") == "Swinging");
  CHECK(TitleFromFile("Wall.jpg") == "Wall");
  CHECK(TitleFromFile("KONG_Big_Wall.jpg") == "Big Wall");
  CHECK(HasExtension("A.JPG", ".jpg") && !HasExtension(".jpg", ".jpg") && !HasExtension("a.jpeg", ".jpg"));
  CHECK(NumberedName("KONG_Rex.jpg", 1) == "KONG_Rex.jpg" && NumberedName("KONG_Rex.jpg", 2) == "KONG_Rex (2).jpg");
  CHECK(NumberedName("noext", 3) == "noext (3)");

  // XDG pictures folder.
  CHECK(XdgPicturesDir("", "/home/k") == "/home/k/Pictures");
  CHECK(XdgPicturesDir("# comment\nXDG_DESKTOP_DIR=\"$HOME/Desktop\"\nXDG_PICTURES_DIR=\"$HOME/Bilder\"\n", "/home/k") ==
        "/home/k/Bilder");
  CHECK(XdgPicturesDir("XDG_PICTURES_DIR=\"/mnt/pics/\"\r\n", "/home/k/") == "/mnt/pics");
  CHECK(XdgPicturesDir("XDG_PICTURES_DIR=\"$HOME/\"", "/home/k") == "/home/k/Pictures");  // disabled
  CHECK(XdgPicturesDir("XDG_PICTURES_DIR=\"relative\"", "/home/k") == "/home/k/Pictures");
  CHECK(XdgPicturesDir("XDG_PICTURES_DIR=\"$HOME/My \\\"Pics\\\"\"", "/home/k") == "/home/k/My \"Pics\"");

  int w = 0, h = 0;
  CHECK(JpegSize(kTestJpeg, sizeof(kTestJpeg), w, h) && w == 32 && h == 16);
  CHECK(!JpegSize(kTestPng, sizeof(kTestPng), w, h));
}

static bool Decodes(const fs::path& file) {
  const std::string s = ReadFile(file);
  return bool(kk::workshop::DecodePicture(std::vector<uint8_t>(s.begin(), s.end()), 64));
}

static void Files(const fs::path& work) {
  const std::string doc = MakeDoc("A synthetic readme.\r", u"");
  std::vector<std::pair<std::string, std::string>> pics;
  for (const char* n : {"KONG_Courtyard.jpg", "KONG_Gates.jpg", "KONG_Reveal.jpg", "KONG_Rex.jpg", "KONG_Swinging.jpg",
                        "KONG_Wall.jpg"})
    pics.push_back({n, Jpeg()});
  const std::string iso = MakeIso(DiscTree(pics, doc));
  const std::string raw = MakeRaw(iso);
  WriteFile(work / "disc.bin", raw);
  std::string why;
  uint64_t last = 0;
  CHECK(ConvertToIso(work / "disc.bin", work / "disc.iso", [&](uint64_t d) { last = d; return true; }, why));
  CHECK(last == raw.size() && ReadFile(work / "disc.iso") == iso);
  WriteFile(work / "odd.bin", raw.substr(0, raw.size() - 1));
  CHECK(!ConvertToIso(work / "odd.bin", work / "odd.iso", nullptr, why) && why.find("2352") != std::string::npos);
  WriteFile(work / "form2.bin", MakeRaw(iso, false, 20));
  CHECK(!ConvertToIso(work / "form2.bin", work / "form2.iso", nullptr, why) && why.find("Form 2") != std::string::npos);
  CHECK(!ConvertToIso(work / "disc.bin", work / "stop.iso", [](uint64_t) { return false; }, why) && why == "stopped");

  // Extraction: six pictures and the readme, nothing else (no screensaver installer).
  const fs::path out = work / "out";
  fs::create_directories(out);
  Extracted got;
  CHECK(ExtractArtbook(work / "disc.iso", out, Decodes, nullptr, got, why));
  CHECK(got.pictures.size() == 6 && got.readme);
  CHECK(ReadFile(out / "KONG_Rex.jpg") == Jpeg());
  CHECK(ReadmeText(out) == "A synthetic readme.");
  CHECK(Paintings(out).size() == 6 && Paintings(out)[0].filename() == "KONG_Courtyard.jpg" && Installed(out));
  size_t files = 0;
  for (auto& e : fs::directory_iterator(out)) files += e.is_regular_file();
  CHECK(files == 7 && !fs::exists(out / "King_Kong_Install.exe"));
  // A picture that does not decode fails the whole thing; the default check reads the JPEG header.
  std::string broken = Jpeg();
  broken.resize(20);  // the start of the JPEG, then no frame header or data (stb_image accepts a merely truncated one;
                      // the whole disc's SHA-256 covers truncation)
  auto pics2 = pics;
  pics2[3].second = broken + std::string(64, '\0');
  WriteFile(work / "bad.iso", MakeIso(DiscTree(pics2, doc)));
  fs::create_directories(work / "out2");
  CHECK(!ExtractArtbook(work / "bad.iso", work / "out2", Decodes, nullptr, got, why) && why.find("KONG_Rex") != std::string::npos);
  pics2[3].second = "not a jpeg at all";
  WriteFile(work / "bad2.iso", MakeIso(DiscTree(pics2, doc)));
  fs::create_directories(work / "out3");
  CHECK(!ExtractArtbook(work / "bad2.iso", work / "out3", nullptr, nullptr, got, why));
  // Unsafe names on the disc are refused.
  auto pics3 = pics;
  pics3[0].first = "../KONG_Escape.jpg";
  WriteFile(work / "unsafe.iso", MakeIso(DiscTree(pics3, doc)));
  fs::create_directories(work / "out4");
  CHECK(!ExtractArtbook(work / "unsafe.iso", work / "out4", nullptr, nullptr, got, why) &&
        why.find("unsafe") != std::string::npos);
  CHECK(!fs::exists(work / "KONG_Escape.jpg"));
  // No art folder: fails; no readme: still fine.
  WriteFile(work / "none.iso", MakeIso(Dir("", "", {Dir("Other", "OTHER", {})})));
  fs::create_directories(work / "out5");
  CHECK(!ExtractArtbook(work / "none.iso", work / "out5", nullptr, nullptr, got, why));

  // Save to PC into a test Pictures folder.
  const fs::path pictures = work / "Pictures (test)";
#if defined(_WIN32)
  _putenv_s("KK_DEV_ARTBOOK_PICTURES", pictures.string().c_str());
#else
  setenv("KK_DEV_ARTBOOK_PICTURES", pictures.string().c_str(), 1);
#endif
  CHECK(PicturesDir() == pictures);
  CHECK(SaveFolder() == pictures / "King Kong Artbook");
  Saved s = SavePicture(out / "KONG_Rex.jpg", SaveFolder());
  CHECK(s.ok && !s.already && s.file == SaveFolder() / "KONG_Rex.jpg" && ReadFile(s.file) == Jpeg());
  s = SavePicture(out / "KONG_Rex.jpg", SaveFolder());
  CHECK(s.ok && s.already && s.file == SaveFolder() / "KONG_Rex.jpg");
  WriteFile(work / "other" / "KONG_Rex.jpg", "a different picture");
  s = SavePicture(work / "other" / "KONG_Rex.jpg", SaveFolder());
  CHECK(s.ok && !s.already && s.file == SaveFolder() / "KONG_Rex (2).jpg");
  CHECK(!SavePicture(out / "missing.jpg", SaveFolder()).ok);
  CHECK(!SavePicture(out / "KONG_Rex.jpg", fs::path()).ok);
  CHECK(!fs::exists(SaveFolder() / "KONG_Rex.jpg.part"));
#if defined(_WIN32)
  _putenv_s("KK_DEV_ARTBOOK_PICTURES", "");
#else
  unsetenv("KK_DEV_ARTBOOK_PICTURES");
#endif
  const fs::path real = PicturesDir();  // only looked at, never written
  CHECK(!real.empty() && real != pictures);
  std::cout << "real Pictures folder: " << real.string() << "\n";

  // The automatic download's memory.
  const fs::path user = work / "user-auto";
  CHECK(!ReadAuto(user).wanted);
  CHECK(WriteAuto(user, {true, 2, false}));
  AutoState a = ReadAuto(user);
  CHECK(a.wanted && a.fails == 2 && !a.told);
  CHECK(AutoFile(user) == user / "content" / "downloads" / "artbook.auto");
  ClearAuto(user);
  CHECK(!fs::exists(AutoFile(user)) && !ReadAuto(user).wanted);
}

// --------------------------------------------------------- service, fake server ---
class FakeFetcher : public kk::content::Fetcher {
 public:
  std::string data;
  std::vector<uint64_t> ranges;
  bool Probe(const std::string&, uint64_t& total, std::string&) override {
    total = data.size();
    return true;
  }
  Result Fetch(const std::string&, const fs::path& part, uint64_t total, const std::atomic<bool>& stop,
               std::atomic<uint64_t>& have) override {
    uint64_t kept = fs::exists(part) ? fs::file_size(part) : 0;
    ranges.push_back(kept);
    std::ofstream f(part, std::ios::binary | std::ios::app);
    while (kept < total) {
      if (stop) return Result::kStopped;
      const size_t n = size_t(std::min<uint64_t>(65536, total - kept));
      f.write(data.data() + kept, std::streamsize(n));
      f.flush();
      kept += n;
      have = kept;
      if (const uint64_t drop = drop_at.load(); drop && kept >= drop) {
        drop_at = 0;
        return Result::kConnection;
      }
    }
    return Result::kComplete;
  }
};

static bool WaitFor(const std::function<bool()>& done, double seconds = 30) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(seconds * 1000));
  while (std::chrono::steady_clock::now() < end) {
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

static void ServiceTests(const fs::path& work) {
  using kk::content::Kind;
  using kk::content::Stage;
  CHECK(kk::content::Quiet(Kind::kArtbook) && !kk::content::Quiet(Kind::kTexturePack));
  const auto& info = kk::content::InfoFor(Kind::kArtbook);
  CHECK(std::string(info.url) == kDiscUrl && info.zip_size == kDiscSize && std::string(info.sha256) == kDiscSha256);
  kk::content::SetPictureCheck(Decodes);
  std::vector<std::pair<std::string, std::string>> pics;
  for (const char* n : {"KONG_Courtyard.jpg", "KONG_Gates.jpg", "KONG_Reveal.jpg", "KONG_Rex.jpg", "KONG_Swinging.jpg",
                        "KONG_Wall.jpg"})
    pics.push_back({n, Jpeg()});
  const std::string raw = MakeRaw(MakeIso(DiscTree(pics, MakeDoc("The readme.\r", u""))));
  const fs::path user = work / "user";
  {
    auto fake = std::make_unique<FakeFetcher>();
    auto* f = fake.get();
    f->data = raw;
    f->drop_at = raw.size() / 2;  // a lost connection on the way: continued with a range
    kk::content::Service s;
    s.OverrideUrl(Kind::kArtbook, "fake://disc", Sha(raw));
    s.Init(user, std::move(fake));
    CHECK(!s.Get(Kind::kArtbook).installed);
    s.Download(Kind::kArtbook);
    CHECK(WaitFor([&] { return s.Idle() && s.Get(Kind::kArtbook).stage == Stage::kInstalled; }));
    CHECK(f->ranges.size() == 2 && f->ranges[1] >= raw.size() / 2);
    const auto st = s.Get(Kind::kArtbook);
    const fs::path dir = kk::content::InstallDir(user, Kind::kArtbook);
    CHECK(st.installed && st.folder == dir && dir == user / "content" / "artbook");
    CHECK(Paintings(dir).size() == 6 && ReadmeText(dir) == "The readme.");
    const fs::path dl = user / "content" / "downloads";
    CHECK(!fs::exists(dl / "artbook.bin") && !fs::exists(dl / "artbook.part") && !fs::exists(dl / "artbook.iso") &&
          !fs::exists(dl / "artbook.state"));
    CHECK(!fs::exists(user / "content" / "artbook.new"));
    CHECK(s.TakeChanges().empty());  // no settings
    s.Download(Kind::kArtbook);      // installed: nothing to do
    CHECK(s.Idle());
  }
  {  // The next start sees it installed.
    auto fake = std::make_unique<FakeFetcher>();
    kk::content::Service s;
    s.Init(user, std::move(fake));
    CHECK(s.Get(Kind::kArtbook).installed && s.Get(Kind::kArtbook).stage == Stage::kInstalled);
    CHECK(s.TakeChanges().empty());
    s.Remove(Kind::kArtbook);
    CHECK(WaitFor([&] { return s.Idle(); }));
    CHECK(!s.Get(Kind::kArtbook).installed && !fs::exists(kk::content::InstallDir(user, Kind::kArtbook)));
  }
  {  // A disc whose SHA-256 is right but that is no disc: failed, the .bin deleted (fetched again next time).
    std::string junk(2352 * 30, 'j');
    auto fake = std::make_unique<FakeFetcher>();
    fake->data = junk;
    kk::content::Service s;
    s.OverrideUrl(Kind::kArtbook, "fake://junk", Sha(junk));
    s.Init(work / "user-junk", std::move(fake));
    s.Download(Kind::kArtbook);
    CHECK(WaitFor([&] { return s.Idle() && s.Get(Kind::kArtbook).stage == Stage::kFailed; }));
    CHECK(s.Get(Kind::kArtbook).error == kk::content::Error::kUnpack);
    CHECK(!fs::exists(work / "user-junk" / "content" / "downloads" / "artbook.bin"));
    CHECK(!fs::exists(work / "user-junk" / "content" / "artbook"));
  }
  {  // A wrong SHA-256: kChecksum, nothing kept.
    auto fake = std::make_unique<FakeFetcher>();
    fake->data = raw;
    kk::content::Service s;
    s.OverrideUrl(Kind::kArtbook, "fake://disc", std::string(64, '0'));
    s.Init(work / "user-sha", std::move(fake));
    s.Download(Kind::kArtbook);
    CHECK(WaitFor([&] { return s.Idle() && s.Get(Kind::kArtbook).stage == Stage::kFailed; }));
    CHECK(s.Get(Kind::kArtbook).error == kk::content::Error::kChecksum);
  }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: kk-artbook-tests <work folder>\n";
    return 2;
  }
  const fs::path work = argv[1];
  std::error_code ec;
  if (fs::exists(work)) {  // a previous run's folder: moved aside, never deleted
    fs::path aside = work;
    aside += ".old-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    fs::rename(work, aside, ec);
  }
  fs::create_directories(work);
  Pure();
  Files(work / "files");
  ServiceTests(work / "service");
  std::cout << "artbook: " << checks << " checks passed\n";
  return 0;
}
