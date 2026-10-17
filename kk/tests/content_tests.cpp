// Content downloads (content.h, content_logic.h) without the network or game data: SHA-256, the range
// replies, ZIP directories (Zip64 too), unsafe names (zip-slip), the texture pack and cutscene checks,
// free space, the folders per OS, and the whole service with a fake server: download, a lost
// connection, pause and continue, closing mid-download and continuing at the next start, the checksum,
// unpacking with the system tool, the settings it applies, Import and Remove.
// Usage: kk-content-tests <empty work folder>
// Optional: KK_CONTENT_TEST_SERVER=http://127.0.0.1:<port> serving test-pack.zip with ranges (see
// kk/out/taskJ1/range_server.py) runs the real fetcher (WinHTTP / curl) against it too.
#include "content.h"
#include "content_logic.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>

namespace fs = std::filesystem;
using namespace kk::content;

static int checks;
#define CHECK(x)                                                     \
  do {                                                               \
    ++checks;                                                        \
    if (!(x)) {                                                      \
      std::cerr << "FAIL line " << __LINE__ << ": " #x "\n";         \
      std::exit(1);                                                  \
    }                                                                \
  } while (0)

// ------------------------------------------------------------- test ZIPs ---
static uint32_t Crc32(const std::string& s) {
  uint32_t c = 0xFFFFFFFF;
  for (unsigned char b : s) {
    c ^= b;
    for (int i = 0; i < 8; ++i) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}
struct ZipFile {
  std::string name, data;
  bool link = false;
};
static void Put16(std::string& o, uint32_t v) { o += char(v & 255); o += char((v >> 8) & 255); }
static void Put32(std::string& o, uint32_t v) { Put16(o, v & 0xFFFF); Put16(o, v >> 16); }
static void Put64(std::string& o, uint64_t v) { Put32(o, uint32_t(v)); Put32(o, uint32_t(v >> 32)); }
// A stored (uncompressed) ZIP; `zip64` writes the Zip64 end records and size fields.
static std::string MakeZip(const std::vector<ZipFile>& files, bool zip64 = false) {
  std::string out, dir;
  for (const auto& f : files) {
    const uint32_t offset = uint32_t(out.size());
    const uint32_t crc = Crc32(f.data);
    Put32(out, 0x04034b50); Put16(out, 20); Put16(out, 0); Put16(out, 0); Put16(out, 0); Put16(out, 0);
    Put32(out, crc); Put32(out, uint32_t(f.data.size())); Put32(out, uint32_t(f.data.size()));
    Put16(out, uint16_t(f.name.size())); Put16(out, 0);
    out += f.name;
    out += f.data;
    std::string extra;
    if (zip64) {
      Put16(extra, 1); Put16(extra, 24);
      Put64(extra, f.data.size()); Put64(extra, f.data.size()); Put64(extra, offset);
    }
    Put32(dir, 0x02014b50); Put16(dir, f.link ? (3 << 8) | 20 : 20); Put16(dir, 20); Put16(dir, 0); Put16(dir, 0);
    Put16(dir, 0); Put16(dir, 0); Put32(dir, crc);
    Put32(dir, zip64 ? 0xFFFFFFFF : uint32_t(f.data.size())); Put32(dir, zip64 ? 0xFFFFFFFF : uint32_t(f.data.size()));
    Put16(dir, uint16_t(f.name.size())); Put16(dir, uint16_t(extra.size())); Put16(dir, 0); Put16(dir, 0); Put16(dir, 0);
    Put32(dir, f.link ? (0120777u << 16) : 0);
    Put32(dir, zip64 ? 0xFFFFFFFF : offset);
    dir += f.name;
    dir += extra;
  }
  const uint64_t dir_offset = out.size();
  out += dir;
  if (zip64) {
    const uint64_t rec = out.size();
    Put32(out, 0x06064b50); Put64(out, 44); Put16(out, 45); Put16(out, 45); Put32(out, 0); Put32(out, 0);
    Put64(out, files.size()); Put64(out, files.size()); Put64(out, dir.size()); Put64(out, dir_offset);
    Put32(out, 0x07064b50); Put32(out, 0); Put64(out, rec); Put32(out, 1);
  }
  Put32(out, 0x06054b50); Put16(out, 0); Put16(out, 0);
  Put16(out, zip64 ? 0xFFFF : uint16_t(files.size())); Put16(out, zip64 ? 0xFFFF : uint16_t(files.size()));
  Put32(out, zip64 ? 0xFFFFFFFF : uint32_t(dir.size())); Put32(out, zip64 ? 0xFFFFFFFF : uint32_t(dir_offset));
  Put16(out, 0);
  return out;
}
static std::vector<ZipEntry> Directory(const std::string& zip, bool* ok = nullptr) {
  std::vector<ZipEntry> entries;
  std::string error;
  auto read_at = [&](uint64_t at, uint8_t* out, size_t n) {
    if (at + n > zip.size()) return false;
    std::memcpy(out, zip.data() + at, n);
    return true;
  };
  const bool r = ReadZipDirectory(read_at, zip.size(), entries, error);
  if (ok) *ok = r;
  return entries;
}
static std::string Sha(const std::string& s) {
  Sha256 h;
  h.Update(s.data(), s.size());
  return h.HexDigest();
}
static void WriteFile(const fs::path& p, const std::string& data) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary).write(data.data(), std::streamsize(data.size()));
}
static std::string ReadFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}

// A small texture pack: pack.txt and three images, inside a folder.
static std::vector<ZipFile> PackFiles(const std::string& folder = "test-pack/", const std::string& first = "KKTP 1") {
  std::vector<ZipFile> files;
  if (!folder.empty()) files.push_back({folder, ""});
  std::string manifest = first + "\r\n# comment\r\n\r\n";
  for (int i = 0; i < 3; ++i) {
    const std::string name = "000000000000000" + std::to_string(i) + "__bf_FF800105_0000000" + std::to_string(i) + ".dds";
    manifest += "000000000000000" + std::to_string(i) + " bf_FF800105_0000000" + std::to_string(i) +
                " BC1_UNORM 16 16 " + name + "\r\n";
    files.push_back({folder + name, std::string(3000 + i * 1000, char('a' + i))});
  }
  files.push_back({folder + "pack.txt", manifest});
  return files;
}

// --------------------------------------------------------------- fake server ---
// Serves `data` like the real server: appends to the partial file from its size, can drop the
// connection once, can ignore the range once, counts the ranges asked for.
class FakeFetcher : public Fetcher {
 public:
  std::string data;
  std::map<std::string, std::string> by_url;  // other files, by address
  std::vector<uint64_t> ranges;  // the start of each request
  int ignore_range_once = 0;
  int chunk_ms = 0;              // slow server: ms per 4 KiB
  std::atomic<int> requests{0};
  const std::string& For(const std::string& url) { return by_url.count(url) ? by_url[url] : data; }
  bool Probe(const std::string& url, uint64_t& total, std::string&) override {
    total = For(url).size();
    return true;
  }
  Result Fetch(const std::string& url, const fs::path& part, uint64_t total, const std::atomic<bool>& stop,
               std::atomic<uint64_t>& have) override {
    const std::string& data = For(url);
    ++requests;
    uint64_t kept = fs::exists(part) ? fs::file_size(part) : 0;
    ranges.push_back(kept);
    if (ignore_range_once && kept) {
      --ignore_range_once;
      return Result::kRestart;  // what a 200 reply to a range request turns into (DecideRangeReply)
    }
    std::ofstream f(part, std::ios::binary | std::ios::app);
    while (kept < total) {
      if (stop) return Result::kStopped;
      const size_t n = size_t(std::min<uint64_t>(4096, total - kept));
      f.write(data.data() + kept, std::streamsize(n));
      f.flush();
      kept += n;
      have = kept;
      if (const uint64_t drop = drop_at.load(); drop && kept >= drop) {
        drop_at = 0;
        return Result::kConnection;
      }
      if (chunk_ms) std::this_thread::sleep_for(std::chrono::milliseconds(chunk_ms));
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

static std::map<std::string, std::string> Apply(Service& s, std::map<std::string, std::string> settings) {
  for (const auto& c : s.TakeChanges()) {
    if (!c.when_name.empty() && settings[c.when_name] != c.when_value) continue;
    if (c.reset) settings.erase(c.name);
    else settings[c.name] = c.value;
  }
  return settings;
}

static void PureTests() {
  // SHA-256 (FIPS 180-2 vectors), in one piece and split at odd places.
  CHECK(Sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(Sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(Sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  {
    const std::string million(1000000, 'a');
    Sha256 h;
    for (size_t at = 0; at < million.size();) {
      const size_t n = std::min<size_t>(million.size() - at, 1 + (at * 7919) % 333);
      h.Update(million.data() + at, n);
      at += n;
    }
    CHECK(h.HexDigest() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  }

  // Range replies.
  CHECK(RangeValue(0) == "bytes=0-");
  CHECK(RangeValue(2561065735ull) == "bytes=2561065735-");
  auto r = ParseContentRange("bytes 0-0/2561065736");
  CHECK(r.ok && r.first == 0 && r.last == 0 && r.total == 2561065736ull);
  r = ParseContentRange(" Bytes 100-199/200");
  CHECK(r.ok && r.first == 100 && r.last == 199);
  r = ParseContentRange("bytes */1871168114");
  CHECK(r.ok && r.unsatisfied && r.total == 1871168114ull);
  CHECK(!ParseContentRange("bytes 0-0/*").ok);
  CHECK(!ParseContentRange("bytes 5-4/10").ok);
  CHECK(!ParseContentRange("bytes 0-10/10").ok);
  CHECK(!ParseContentRange("items 0-0/10").ok);
  CHECK(!ParseContentRange("").ok);
  CHECK(SizeFromProbe(206, ParseContentRange("bytes 0-0/2561065736"), 1) == 2561065736ull);
  CHECK(SizeFromProbe(200, {}, 1234) == 1234);
  CHECK(SizeFromProbe(404, {}, 1234) == 0);  // the server's answer to HEAD
  CHECK(DecideRangeReply(0, 206, ParseContentRange("bytes 0-99/100")) == RangeReply::kAppend);
  CHECK(DecideRangeReply(50, 206, ParseContentRange("bytes 50-99/100")) == RangeReply::kAppend);
  CHECK(DecideRangeReply(50, 206, ParseContentRange("bytes 0-99/100")) == RangeReply::kFail);
  CHECK(DecideRangeReply(0, 200, {}) == RangeReply::kAppend);
  CHECK(DecideRangeReply(50, 200, {}) == RangeReply::kRestart);
  CHECK(DecideRangeReply(100, 416, ParseContentRange("bytes */100")) == RangeReply::kComplete);
  CHECK(DecideRangeReply(120, 416, ParseContentRange("bytes */100")) == RangeReply::kRestart);
  CHECK(DecideRangeReply(50, 404, {}) == RangeReply::kFail);
  CHECK(DecideRangeReply(50, 503, {}) == RangeReply::kFail);

  // Unsafe names (zip-slip and names Windows would change).
  for (const char* good : {"kk-4x-usv2/", "kk-4x-usv2/pack.txt", "a/b/c.dds", "KK_Cuts_AI/upscaled_only/BK_0105.mp4",
                           "x.y.z", "console/ok.dds"})
    CHECK(SafeEntryName(good));
  for (const char* bad : {"", "/", "/etc/passwd", "../evil", "a/../../evil", "a/..", "..", "./a", "a/./b", "a//b",
                          "C:/Windows/x", "c:evil", "a\\..\\b", "a\\b", "dir/file.", "dir /x", "CON", "aux.txt",
                          "x/NUL.dds", "com1/a", "a:stream", "bad\x01name", "a?b", "a*b", "a|b"})
    CHECK(!SafeEntryName(bad));

  // ZIP directories.
  bool ok = false;
  const auto files = PackFiles();
  auto entries = Directory(MakeZip(files), &ok);
  CHECK(ok && entries.size() == files.size());
  CHECK(entries[0].directory && entries[0].name == "test-pack/");
  CHECK(entries[1].size == 3000 && entries[1].crc == Crc32(files[1].data));
  auto entries64 = Directory(MakeZip(files, /*zip64=*/true), &ok);
  CHECK(ok && entries64.size() == files.size() && entries64[2].size == 4000 && entries64[2].packed == 4000);
  Directory("not a zip at all, just some text that is long enough", &ok);
  CHECK(!ok);
  std::string cut = MakeZip(files);
  cut.resize(cut.size() - 30);
  Directory(cut, &ok);
  CHECK(!ok);
  CHECK(Directory(MakeZip({{"a", "x", /*link=*/true}}))[0].link);

  // The texture pack layout.
  auto layout = CheckPackLayout(entries);
  CHECK(layout.problem == Problem::kNone && layout.folder == "test-pack" && layout.files == 4);
  CHECK(layout.unpacked == 3000 + 4000 + 5000 + files.back().data.size());
  CHECK(CheckPackLayout(Directory(MakeZip(PackFiles("")))).problem == Problem::kNone);
  CHECK(CheckPackLayout(Directory(MakeZip(PackFiles("")))).folder.empty());
  CHECK(CheckPackLayout(Directory(MakeZip(PackFiles("outer/inner/")))).folder == "outer/inner");
  {
    auto f = PackFiles();
    f.push_back({"readme.txt", "hello"});  // outside the pack folder: not installed, allowed
    CHECK(CheckPackLayout(Directory(MakeZip(f))).problem == Problem::kNone);
    f.push_back({"test-pack/sub/x.dds", "x"});
    CHECK(CheckPackLayout(Directory(MakeZip(f))).problem == Problem::kSubfolders);
  }
  {
    auto f = PackFiles();
    f.push_back({"test-pack/sub/", ""});
    CHECK(CheckPackLayout(Directory(MakeZip(f))).problem == Problem::kSubfolders);
  }
  {
    auto f = PackFiles();
    f.push_back({"other/pack.txt", "KKTP 1\n"});
    CHECK(CheckPackLayout(Directory(MakeZip(f))).problem == Problem::kManyManifests);
  }
  CHECK(CheckPackLayout(Directory(MakeZip({{"a/b.dds", "x"}}))).problem == Problem::kNoManifest);
  CHECK(CheckPackLayout(Directory(MakeZip({{"../pack.txt", "KKTP 1\n"}}))).problem == Problem::kUnsafeName);
  CHECK(CheckPackLayout(Directory(MakeZip({{"p/pack.txt", "KKTP 1\n"}, {"p/x.dds", "x", true}}))).problem ==
        Problem::kUnsafeName);
  {
    auto big = entries;
    big[1].size = kMaxImageBytes + 1;
    CHECK(CheckPackLayout(big).problem == Problem::kTooLarge);
  }
  CHECK(CheckNames({}).problem == Problem::kNotZip);

  // pack.txt.
  std::set<std::string> have = {"a.dds", "b.png"};
  auto has = [&](const std::string& f) { return have.count(f) != 0; };
  size_t rows = 0;
  CHECK(CheckManifest("KKTP 1\n# x\n\n0 bf 1 2 3 a.dds\r\n  0 bf 1 2 3 b.png\n", has, &rows) == Problem::kNone && rows == 2);
  CHECK(CheckManifest("KKTP 1", has) == Problem::kNone);
  CHECK(CheckManifest("KKTP 2\n0 bf 1 2 3 a.dds\n", has) == Problem::kBadManifest);
  CHECK(CheckManifest("\xEF\xBB\xBFKKTP 1\n", has) == Problem::kBadManifest);  // BOM: the loader rejects it
  CHECK(CheckManifest("", has) == Problem::kBadManifest);
  CHECK(CheckManifest("KKTP 1\n0 bf 1 2 a.dds\n", has) == Problem::kBadManifest);
  CHECK(CheckManifest("KKTP 1\n0 bf 1 2 3 c.dds\n", has) == Problem::kMissingFile);
  CHECK(CheckManifest("KKTP 1\n0 bf 1 2 3 sub/a.dds\n", has) == Problem::kBadManifest);
  CHECK(CheckManifest("KKTP 1\n0 bf 1 2 3 ..\\a.dds\n", has) == Problem::kBadManifest);
  CHECK(CheckManifest("KKTP 1\n0 bf 1 2 3 a.tga\n", has) == Problem::kBadManifest);

  // The cutscene layout: the folder with the most BK_xxxx.mp4.
  CHECK(IsMovieName("BK_0105.mp4") && IsMovieName("bk_00ab.MP4") && !IsMovieName("BK_010.mp4") &&
        !IsMovieName("BK_01G5.mp4") && !IsMovieName("XK_0105.mp4"));
  auto movies = ChooseMovieFolder(Directory(MakeZip({{"KK_Cuts_AI/", ""},
                                                     {"KK_Cuts_AI/AI_expanded/BK_0105.mp4", "e"},
                                                     {"KK_Cuts_AI/upscaled_only/BK_0105.mp4", "u"},
                                                     {"KK_Cuts_AI/upscaled_only/BK_0201.mp4", "u"},
                                                     {"KK_Cuts_AI/readme.txt", "r"}})));
  CHECK(movies.problem == Problem::kNone && movies.folder == "KK_Cuts_AI/upscaled_only" && movies.files == 2);
  CHECK(ChooseMovieFolder(Directory(MakeZip({{"a/readme.txt", "r"}}))).problem == Problem::kNoMovies);
  CHECK(ChooseMovieFolder(Directory(MakeZip({{"a/../BK_0105.mp4", "r"}}))).problem == Problem::kUnsafeName);

  // Free space and time left.
  CHECK(SpaceNeeded(2561065736ull, 0, 5322454913ull) == 2561065736ull + 5322454913ull + kSpaceMargin);
  CHECK(SpaceNeeded(100, 60, 500) == 40 + 500 + kSpaceMargin);
  CHECK(SpaceNeeded(100, 160, 500) == 500 + kSpaceMargin);
  CHECK(SecondsLeft(50, 150, 10) == 10);
  CHECK(SecondsLeft(50, 150, 0) < 0);
  CHECK(SecondsLeft(150, 150, 0) == 0);

  // Where content goes, per OS: under the user folder the SDK chose (content.h).
  CHECK(InstallDir(fs::path("C:/Users/Player/Documents/king_kong"), Kind::kTexturePack).generic_string() ==
        "C:/Users/Player/Documents/king_kong/content/hd-textures");
  CHECK(InstallDir(fs::path("/home/deck/.local/share/king_kong"), Kind::kCutscenes).generic_string() ==
        "/home/deck/.local/share/king_kong/content/ps2-cutscenes");
  CHECK(ContentRoot(fs::path("/x")).generic_string() == "/x/content");
  CHECK(std::string(InfoFor(Kind::kTexturePack).sha256).size() == 64 && InfoFor(Kind::kCutscenes).zip_size == 1871168114ull);
}

static void ServiceTests(const fs::path& work) {
  // Download -> lost connection -> continues with a range -> SHA-256 -> unpack -> settings.
  std::string zip = MakeZip(PackFiles());
  const fs::path user = work / "user";
  {
    auto fake = std::make_unique<FakeFetcher>();
    auto* f = fake.get();
    f->data = zip;
    f->drop_at = zip.size() / 3;
    Service s;
    s.OverrideUrl(Kind::kTexturePack, "fake://pack", Sha(zip));
    s.Init(user, std::move(fake));
    CHECK(s.Get(Kind::kTexturePack).stage == Stage::kNone);
    s.Download(Kind::kTexturePack);
    CHECK(WaitFor([&] { return s.Idle() && s.Get(Kind::kTexturePack).stage == Stage::kInstalled; }));
    CHECK(f->ranges.size() == 2 && f->ranges[0] == 0 && f->ranges[1] >= zip.size() / 3);
    const auto st = s.Get(Kind::kTexturePack);
    CHECK(st.installed && !st.imported && st.installed_name == "test-pack");
    CHECK(st.folder == InstallDir(user, Kind::kTexturePack) / "test-pack");
    CHECK(fs::exists(st.folder / "pack.txt") && fs::file_size(st.folder / "0000000000000002__bf_FF800105_00000002.dds") == 5000);
    CHECK(!fs::exists(user / "content" / "downloads" / "hd-textures.zip"));   // the ZIP is deleted
    CHECK(!fs::exists(user / "content" / "downloads" / "hd-textures.part"));
    CHECK(!fs::exists(user / "content" / "hd-textures.new"));
    auto settings = Apply(s, {{"native_texture_pack", "false"}});
    CHECK(settings["native_texture_pack"] == "true");
    CHECK(fs::path(std::u8string((const char8_t*)settings["native_texture_pack_folder"].c_str())) == st.folder);
    CHECK(s.TakeChanges().empty());
    CHECK(ReadFile(InstallDir(user, Kind::kTexturePack) / "installed.txt").find("applied=1") != std::string::npos);

    // Remove: folder gone, settings back (only while they point at it).
    s.Remove(Kind::kTexturePack);
    CHECK(WaitFor([&] { return s.Idle(); }));
    CHECK(!fs::exists(InstallDir(user, Kind::kTexturePack)) && !s.Get(Kind::kTexturePack).installed);
    settings = Apply(s, settings);
    CHECK(settings["native_texture_pack"] == "false" && !settings.count("native_texture_pack_folder"));
  }

  // Server ignores the range once (200): starts again from 0 and still ends right.
  {
    auto fake = std::make_unique<FakeFetcher>();
    auto* f = fake.get();
    f->data = zip;
    f->drop_at = zip.size() / 2;
    f->ignore_range_once = 1;
    Service s;
    s.OverrideUrl(Kind::kTexturePack, "fake://pack", Sha(zip));
    s.Init(work / "user-restart", std::move(fake));
    s.Download(Kind::kTexturePack);
    CHECK(WaitFor([&] { return s.Idle() && s.Get(Kind::kTexturePack).installed; }));
    CHECK(f->ranges.size() == 3 && f->ranges[1] > 0 && f->ranges[2] == 0);
  }

  // Pause and continue; then close mid-download and continue at the next start (a bigger ZIP, served
  // slowly, so there is time to stop it half way).
  {
    auto files = PackFiles();
    files.push_back({"test-pack/extra.bin", std::string(600000, 'x')});
    zip = MakeZip(files);
  }
  const fs::path user2 = work / "user2";
  {
    auto fake = std::make_unique<FakeFetcher>();
    auto* f = fake.get();
    f->data = zip;
    f->chunk_ms = 15;
    Service s;
    s.OverrideUrl(Kind::kTexturePack, "fake://pack", Sha(zip));
    s.Init(user2, std::move(fake));
    s.Download(Kind::kTexturePack);
    CHECK(WaitFor([&] { return s.Get(Kind::kTexturePack).done >= 4096; }));
    s.Pause(Kind::kTexturePack);
    CHECK(WaitFor([&] { return s.Idle(); }));
    auto st = s.Get(Kind::kTexturePack);
    CHECK(st.stage == Stage::kPaused && st.done >= 4096 && st.done < zip.size());
    CHECK(ReadFile(user2 / "content" / "downloads" / "hd-textures.state").find("state=paused") != std::string::npos);
    s.Download(Kind::kTexturePack);  // continue
    CHECK(WaitFor([&] { return s.Get(Kind::kTexturePack).done >= st.done + 4096; }));
    CHECK(f->ranges.size() == 2 && f->ranges[1] == st.done);
    // Closing now (the service stops its worker; the state file still says downloading).
  }
  CHECK(ReadFile(user2 / "content" / "downloads" / "hd-textures.state").find("state=downloading") != std::string::npos);
  const uint64_t kept = fs::file_size(user2 / "content" / "downloads" / "hd-textures.part");
  CHECK(kept > 0 && kept < zip.size());
  {
    auto fake = std::make_unique<FakeFetcher>();
    auto* f = fake.get();
    f->data = zip;
    Service s;
    s.OverrideUrl(Kind::kTexturePack, "fake://pack", Sha(zip));
    s.Init(user2, std::move(fake));  // continues by itself
    CHECK(WaitFor([&] { return s.Idle() && s.Get(Kind::kTexturePack).installed; }));
    CHECK(!f->ranges.empty() && f->ranges[0] == kept);
    CHECK(Apply(s, {})["native_texture_pack"] == "true");
  }
  // A paused download stays paused at the next start; Cancel deletes it.
  {
    const fs::path user3 = work / "user3";
    auto fake = std::make_unique<FakeFetcher>();
    fake->data = zip;
    fake->chunk_ms = 15;
    {
      Service s;
      s.OverrideUrl(Kind::kTexturePack, "fake://pack", Sha(zip));
      s.Init(user3, std::move(fake));
      s.Download(Kind::kTexturePack);
      CHECK(WaitFor([&] { return s.Get(Kind::kTexturePack).done >= 4096; }));
      s.Pause(Kind::kTexturePack);
      CHECK(WaitFor([&] { return s.Idle(); }));
    }
    auto fake2 = std::make_unique<FakeFetcher>();
    fake2->data = zip;
    Service s;
    s.Init(user3, std::move(fake2));
    CHECK(s.Get(Kind::kTexturePack).stage == Stage::kPaused && s.Idle());
    s.Cancel(Kind::kTexturePack);
    CHECK(s.Get(Kind::kTexturePack).stage == Stage::kNone);
    CHECK(!fs::exists(user3 / "content" / "downloads" / "hd-textures.part"));
    CHECK(!fs::exists(user3 / "content" / "downloads" / "hd-textures.state"));
  }

  // A download still waiting behind another one also goes on after a restart.
  {
    const fs::path user4 = work / "user4";
    const std::string movies = MakeZip({{"m/BK_0105.mp4", "movie"}});
    auto make = [&](int chunk_ms) {
      auto fake = std::make_unique<FakeFetcher>();
      fake->data = zip;
      fake->by_url["fake://movies"] = movies;
      fake->chunk_ms = chunk_ms;
      return fake;
    };
    {
      Service s;
      s.OverrideUrl(Kind::kTexturePack, "fake://pack", Sha(zip));
      s.OverrideUrl(Kind::kCutscenes, "fake://movies", Sha(movies));
      s.Init(user4, make(15));
      s.Download(Kind::kTexturePack);
      s.Download(Kind::kCutscenes);
      CHECK(s.Get(Kind::kCutscenes).stage == Stage::kQueued);
      CHECK(WaitFor([&] { return s.Get(Kind::kTexturePack).done >= 4096; }));
    }
    CHECK(ReadFile(user4 / "content" / "downloads" / "ps2-cutscenes.state").find("state=downloading") != std::string::npos);
    Service s;
    s.OverrideUrl(Kind::kTexturePack, "fake://pack", Sha(zip));
    s.OverrideUrl(Kind::kCutscenes, "fake://movies", Sha(movies));
    s.Init(user4, make(0));
    CHECK(WaitFor([&] { return s.Idle() && s.Get(Kind::kTexturePack).installed && s.Get(Kind::kCutscenes).installed; }));
    auto settings = Apply(s, {});
    CHECK(settings["native_texture_pack"] == "true" && settings["kk_ps2_cutscenes"] == "true");
  }

  // Wrong checksum: nothing installed, nothing applied, the ZIP deleted.
  {
    auto fake = std::make_unique<FakeFetcher>();
    fake->data = zip;
    Service s;
    s.OverrideUrl(Kind::kTexturePack, "fake://pack", std::string(64, '0'));
    const fs::path u = work / "user-sha";
    s.Init(u, std::move(fake));
    s.Download(Kind::kTexturePack);
    CHECK(WaitFor([&] { return s.Idle() && s.Get(Kind::kTexturePack).stage == Stage::kFailed; }));
    CHECK(s.Get(Kind::kTexturePack).error == Error::kChecksum && !s.Get(Kind::kTexturePack).installed);
    CHECK(s.TakeChanges().empty() && !fs::exists(InstallDir(u, Kind::kTexturePack)));
    CHECK(!fs::exists(u / "content" / "downloads" / "hd-textures.zip"));
  }

  // Cutscenes: the movie folder, the PS2 settings.
  {
    const std::string movies = MakeZip({{"KK_Cuts_AI/", ""}, {"KK_Cuts_AI/AI_expanded/", ""},
                                        {"KK_Cuts_AI/AI_expanded/BK_0105.mp4", "alt"},
                                        {"KK_Cuts_AI/upscaled_only/", ""},
                                        {"KK_Cuts_AI/upscaled_only/BK_0105.mp4", "movie one"},
                                        {"KK_Cuts_AI/upscaled_only/BK_0201.mp4", "movie two"}});
    auto fake = std::make_unique<FakeFetcher>();
    fake->data = movies;
    Service s;
    s.OverrideUrl(Kind::kCutscenes, "fake://movies", Sha(movies));
    const fs::path u = work / "user-movies";
    s.Init(u, std::move(fake));
    s.Download(Kind::kCutscenes);
    CHECK(WaitFor([&] { return s.Idle() && s.Get(Kind::kCutscenes).installed; }));
    const auto st = s.Get(Kind::kCutscenes);
    CHECK(st.folder == InstallDir(u, Kind::kCutscenes) / "upscaled_only");
    CHECK(ReadFile(st.folder / "BK_0201.mp4") == "movie two");
    auto settings = Apply(s, {});
    CHECK(settings["kk_ps2_cutscenes"] == "true" && !settings["kk_ps2_movie_folder"].empty());
    // Remove leaves a folder the player chose themselves alone.
    settings["kk_ps2_movie_folder"] = "D:/my movies";
    s.Remove(Kind::kCutscenes);
    CHECK(WaitFor([&] { return s.Idle(); }));
    settings = Apply(s, settings);
    CHECK(settings["kk_ps2_cutscenes"] == "true" && settings["kk_ps2_movie_folder"] == "D:/my movies");
    CHECK(!fs::exists(InstallDir(u, Kind::kCutscenes)));
  }

  // "I already have a texture pack": good ZIP, then bad ones (nothing applied, nothing left behind, an
  // earlier install kept).
  {
    const fs::path u = work / "user-import";
    Service s;
    s.Init(u, std::make_unique<FakeFetcher>());
    const fs::path good = work / "zips" / "my pack.zip";
    auto files = PackFiles("my-pack/");
    files.push_back({"notes.txt", "not part of the pack"});
    WriteFile(good, MakeZip(files));
    s.Import(good);
    CHECK(WaitFor([&] { return s.Idle() && s.Get(Kind::kTexturePack).installed; }));
    auto st = s.Get(Kind::kTexturePack);
    CHECK(st.imported && st.installed_name == "my-pack" && fs::exists(good));  // the player's ZIP is kept
    CHECK(!fs::exists(InstallDir(u, Kind::kTexturePack) / "notes.txt"));
    CHECK(Apply(s, {})["native_texture_pack"] == "true");
    struct Bad {
      const char* name;
      std::vector<ZipFile> files;
      Problem problem;
    };
    auto missing = PackFiles("p/");
    missing.erase(missing.begin() + 1);  // an image pack.txt lists
    auto subfolder = PackFiles("p/");
    subfolder.push_back({"p/deeper/x.dds", "x"});
    const std::vector<Bad> bad = {
        {"slip.zip", {{"../../evil.txt", "owned"}, {"p/pack.txt", "KKTP 1\n"}}, Problem::kUnsafeName},
        {"abs.zip", {{"/tmp/evil.txt", "owned"}}, Problem::kUnsafeName},
        {"version.zip", PackFiles("p/", "KKTP 2"), Problem::kBadManifest},
        {"missing.zip", missing, Problem::kMissingFile},
        {"subfolder.zip", subfolder, Problem::kSubfolders},
        {"nopack.zip", {{"p/a.dds", "x"}}, Problem::kNoManifest},
    };
    for (const auto& b : bad) {
      const fs::path z = work / "zips" / b.name;
      WriteFile(z, MakeZip(b.files));
      s.Import(z);
      CHECK(WaitFor([&] { return s.Idle(); }));
      st = s.Get(Kind::kTexturePack);
      if (st.stage != Stage::kFailed || st.error != Error::kBadPack || st.problem != b.problem)
        std::cerr << b.name << ": stage " << int(st.stage) << " error " << int(st.error) << " problem " << int(st.problem) << "\n";
      CHECK(st.stage == Stage::kFailed && st.error == Error::kBadPack && st.problem == b.problem);
      CHECK(st.installed && st.installed_name == "my-pack");  // the earlier pack is untouched
      CHECK(s.TakeChanges().empty());
      CHECK(!fs::exists(InstallDir(u, Kind::kTexturePack).string() + ".new"));
    }
    CHECK(!fs::exists(work / "evil.txt") && !fs::exists(work.parent_path() / "evil.txt"));
    WriteFile(work / "zips" / "junk.zip", "this is not a zip file at all............");
    s.Import(work / "zips" / "junk.zip");
    CHECK(WaitFor([&] { return s.Idle(); }));
    CHECK(s.Get(Kind::kTexturePack).problem == Problem::kNotZip);
  }
}

// Optional: the real fetcher (WinHTTP or curl) against a local range server.
static void ServerTest(const fs::path& work, const std::string& server) {
  auto files = PackFiles("server-pack/");
  files.push_back({"server-pack/extra.bin", std::string(5000000, 'y')});  // big enough to drop half way
  const std::string zip = MakeZip(files);
  WriteFile(work / "served" / "test-pack.zip", zip);
  std::cout << "serve " << (work / "served").string() << " at " << server << " (test-pack.zip)\n";
  const std::string url = server + "/test-pack.zip";
  auto fetcher = MakeSystemFetcher();
  uint64_t total = 0;
  std::string detail;
  CHECK(fetcher->Probe(url, total, detail));
  CHECK(total == zip.size());
  Service s;
  s.OverrideUrl(Kind::kTexturePack, url, Sha(zip));
  auto f = MakeSystemFetcher();
  f->drop_at = zip.size() / 2;
  s.Init(work / "user-server", std::move(f));
  s.Download(Kind::kTexturePack);
  CHECK(WaitFor([&] { return s.Idle(); }, 60));
  CHECK(s.Get(Kind::kTexturePack).installed);
  std::cout << "real fetcher: ok\n";
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: kk-content-tests <work folder>\n";
    return 2;
  }
  const fs::path work = fs::absolute(argv[1]);
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work);
  SetLog([](bool warning, const std::string& line) {
    if (std::getenv("KK_CONTENT_TEST_LOG")) std::cerr << (warning ? "W " : "  ") << line << "\n";
  });
  PureTests();
  ServiceTests(work);
  if (const char* server = std::getenv("KK_CONTENT_TEST_SERVER"); server && *server) ServerTest(work, server);
  std::cout << checks << " content checks passed\n";
  return 0;
}
