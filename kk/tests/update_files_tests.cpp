// Task 370: the updater's file work (update_files.h) on real files in a temp folder: kk-files.txt and
// kk-retired.txt parsing and unsafe paths, protected paths, SHA256SUMS.txt lines, the install check, the staged
// install with its undo (a locked file on Windows), retired files (only unchanged ones, empty folders after), the
// first start check's install from a downloaded copy.
//
// Usage: kk-update-tests <scratch folder>
//        kk-update-tests --finish <install folder> <unpacked release folder>
// The second form is the packaging simulation's step (codex\simulate-update.ps1): it runs the new program's first
// start check on an install an old updater left (retire, then the missing or wrong files from the release), then
// prints what it did and whether the install now equals kk-files.txt (exit 0) or not (exit 1).

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "update_files.h"

namespace {

namespace fs = std::filesystem;
using namespace kk::update;

int failures = 0;
void Check(bool ok, const std::string& what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}

void Put(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
}

std::string Get(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string Line(const std::string& path, const std::string& text) {
  return Sha256Of(text.data(), text.size()) + " " + std::to_string(text.size()) + " " + path + "\n";
}

bool Has(const std::vector<std::string>& v, const std::string& s) {
  for (const auto& x : v)
    if (x == s) return true;
  return false;
}

void ParseTests() {
  const std::string good = "\xEF\xBB\xBF# King Kong Recompiled file list\r\nversion 2.0.0-beta.17\r\n" +
                           Line("king_kong.exe", "exe") + Line("launcher/easter-egg/Data Files/a b.bin", "x") + "\n";
  std::string error;
  const auto m = ParseManifest(good, &error);
  Check(m.has_value(), "good list parses: " + error);
  if (m) {
    Check(m->version == "2.0.0-beta.17", "version line");
    Check(m->files.size() == 2, "two files");
    Check(m->files[1].path == "launcher/easter-egg/Data Files/a b.bin", "path with spaces kept");
    Check(m->files[0].size == 3, "size");
    Check(m->Find("KING_KONG.EXE") != nullptr, "Find ignores letter case");
    Check(m->Find("missing.dll") == nullptr, "Find: not listed");
  }
  const std::string sha = std::string(64, 'a');
  for (const char* bad : {"../x", "/x", "a\\b", "c:x", "a//b", "a/./b", "a/../b", "a/", ""}) {
    Check(!ParseManifest(sha + " 1 " + bad + "\n").has_value(), std::string("unsafe path refused: ") + bad);
    Check(!SafePath(bad), std::string("SafePath refuses ") + bad);
  }
  Check(!ParseManifest("abc 1 x\n").has_value(), "short hash refused");
  Check(!ParseManifest(sha + " 1x x\n").has_value(), "bad size refused");
  Check(!ParseManifest(sha + " 1\n").has_value(), "no path refused");
  Check(ParseManifest(std::string(64, 'A') + " 1 x\n")->files[0].sha256 == sha, "hash lowercased");
  Check(SafePath("launcher/video/a.mp4") && SafePath("kk-files.txt"), "SafePath allows normal paths");
}

void ProtectedTests() {
  const Protected keep({"My Data", "games/kk/"});
  for (const char* p : {"game/default.xex", "GAME/x", "logs/a.log", "king_kong.toml", "King_Kong.TOML", "fpe.log",
                        "user/mods/x", "cache/a", "mods/m/mod.toml", "texture-packs/p/pack.txt", "my data/x",
                        "games/kk/default.xex"})
    Check(keep(p), std::string("protected: ") + p);
  for (const char* p : {"king_kong.exe", "launcher/video/a.mp4", "mods-builtin/hardcore/mod.toml", "game",
                        "my datax/y", "glyphs/a.png", "gamex/a"})
    Check(!keep(p), std::string("not protected: ") + p);
}

void SumsTests() {
  const std::string h1 = std::string(64, 'b'), h2 = std::string(63, 'c') + "D";
  const std::string sums = h1 + "  KingKong-v2.0.0-beta.17-windows-x64.zip\r\n" + h2 + " *other.zip\n";
  Check(SumFor(sums, "KingKong-v2.0.0-beta.17-windows-x64.zip") == h1, "SHA256SUMS two spaces");
  Check(SumFor(sums, "other.zip") == std::string(63, 'c') + "d", "SHA256SUMS star form, lowercased");
  Check(SumFor(sums, "missing.zip").empty(), "SHA256SUMS: no line");
  Check(Sha256Of("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 abc");
}

Manifest MakeList(const std::vector<std::pair<std::string, std::string>>& files, const std::string& version = "1") {
  std::string text = "version " + version + "\n";
  for (const auto& [p, t] : files) text += Line(p, t);
  return *ParseManifest(text);
}

void CheckTests(const fs::path& dir) {
  const fs::path root = dir / "check";
  Put(root / "a.dll", "AAAA");
  Put(root / "b.dll", "BBBB");      // same size as listed, other content
  Put(root / "big.bin", "XXXXXXXX");  // same size, other content, above the hash limit used below
  Put(root / "c.txt", "short");
  const Manifest m = MakeList({{"a.dll", "AAAA"}, {"b.dll", "bbbb"}, {"big.bin", "YYYYYYYY"}, {"c.txt", "longer"},
                               {"missing/d.txt", "d"}, {"game/default.xex", "g"}, {std::string(kExtraFilesZip), "z"}});
  const Protected keep;
  auto got = MissingOrWrong(root, m, keep, 4);
  Check(!Has(got, "a.dll"), "check: unchanged file passes");
  Check(Has(got, "b.dll"), "check: small file with other content found by its hash");
  Check(!Has(got, "big.bin"), "check: large file with the right size passes the cheap check");
  Check(Has(got, "c.txt") && Has(got, "missing/d.txt"), "check: wrong size and missing found");
  Check(!Has(got, "game/default.xex"), "check: protected skipped");
  Check(!Has(got, kExtraFilesZip), "check: update-extra-files.zip skipped");
  got = MissingOrWrong(root, m, keep, UINT64_MAX);
  Check(Has(got, "big.bin"), "check: full hash finds the large file");
}

void InstallTests(const fs::path& dir) {
  const fs::path src = dir / "src", dst = dir / "dst";
  Put(src / "king_kong.exe", "new exe");
  Put(src / "launcher/video/a.mp4", "new video");
  Put(src / "newtop/readme.txt", "new folder");
  Put(dst / "king_kong.exe", "old exe");
  Put(dst / "launcher/video/a.mp4", "old video");
  Put(dst / "player.txt", "mine");
  Put(dst / "game/default.xex", "game");
  const std::vector<std::string> plan = {"king_kong.exe", "launcher/video/a.mp4", "newtop/readme.txt"};
  auto r = InstallFiles(src, dst, plan);
  Check(r.outcome == Outcome::kOk, "install: ok");
  Check(Get(dst / "king_kong.exe") == "new exe" && Get(dst / "launcher/video/a.mp4") == "new video" &&
            Get(dst / "newtop/readme.txt") == "new folder",
        "install: every file is the new one, the new top folder too");
  Check(Get(dst / "player.txt") == "mine" && Get(dst / "game/default.xex") == "game", "install: others untouched");
  Check(!fs::exists(dst / "king_kong.exe.old") && !fs::exists(dst / "king_kong.exe.new"), "install: no leftovers");

  // A file the update lacks: nothing changes, no .new files, no new folders.
  Put(dst / "king_kong.exe", "old exe");
  r = InstallFiles(src, dst, {"king_kong.exe", "brandnew/x.txt", "missing.dll"});
  Check(r.outcome == Outcome::kNoSource && r.path == "brandnew/x.txt", "install: missing source refused");
  Check(Get(dst / "king_kong.exe") == "old exe" && !fs::exists(dst / "king_kong.exe.new"),
        "install: nothing changed after a missing source");

#if defined(_WIN32)
  // A locked file in the middle: the files before it were swapped in and must be put back.
  fs::remove_all(dst / "newtop");
  Put(dst / "launcher/video/a.mp4", "old video");
  Put(src / "zzz/after.txt", "after");
  const HANDLE lock = CreateFileW((dst / "launcher/video/a.mp4").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(lock != INVALID_HANDLE_VALUE, "lock test: file opened");
  r = InstallFiles(src, dst, {"king_kong.exe", "newtop/readme.txt", "launcher/video/a.mp4", "zzz/after.txt"});
  CloseHandle(lock);
  Check(r.outcome == Outcome::kInUse && r.path == "launcher/video/a.mp4", "lock: in use reported with its path");
  Check(Get(dst / "king_kong.exe") == "old exe", "lock: the swapped exe is put back");
  Check(Get(dst / "launcher/video/a.mp4") == "old video", "lock: the locked file is unchanged");
  Check(!fs::exists(dst / "newtop") && !fs::exists(dst / "zzz"), "lock: new folders removed again");
  bool leftovers = false;
  for (auto& e : fs::recursive_directory_iterator(dst)) {
    const auto ext = e.path().extension();
    leftovers |= ext == ".new" || ext == ".old" || ext == ".undo";
  }
  Check(!leftovers, "lock: no .new, .old or .undo files left");
  // And the retry after it was closed works.
  r = InstallFiles(src, dst, {"king_kong.exe", "newtop/readme.txt", "launcher/video/a.mp4", "zzz/after.txt"});
  Check(r.outcome == Outcome::kOk && Get(dst / "launcher/video/a.mp4") == "new video", "lock: retry installs");
#endif
}

void RetireTests(const fs::path& dir) {
  const fs::path root = dir / "retire";
  Put(root / "rexgpu-xenos.dll", "xenos 1.9.12");
  Put(root / "mods-builtin/ultrawide/mod.toml", "ultrawide");
  Put(root / "mods-builtin/ultrawide/readme.txt", "ultrawide readme");
  Put(root / "launcher/video/old.mp4", "changed by the player");
  Put(root / "launcher/video/kept.mp4", "still shipped");
  Put(root / "game/old.bin", "game");
  Put(root / "reshade.ini", "player");
  Put(root / "mods-builtin/hardcore/mod.toml", "hardcore");
  Put(root / "olddir/a.txt", "a");
  Put(root / "olddir/player.txt", "player");
  const Manifest retired = MakeList({{"rexgpu-xenos.dll", "xenos 1.9.11"},
                                     {"rexgpu-xenos.dll", "xenos 1.9.12"},
                                     {"mods-builtin/ultrawide/mod.toml", "ultrawide"},
#if defined(_WIN32)  // Windows paths ignore case (Linux updates replace the AppImage and never retire files)
                                     {"Mods-Builtin/Ultrawide/readme.txt", "ultrawide readme"},
#else
                                     {"mods-builtin/ultrawide/readme.txt", "ultrawide readme"},
#endif
                                     {"launcher/video/old.mp4", "original"},
                                     {"launcher/video/kept.mp4", "still shipped"},
                                     {"game/old.bin", "game"},
                                     {"olddir/a.txt", "a"}});
  const Manifest current = MakeList({{"launcher/video/kept.mp4", "still shipped"}});
  const auto removed = RetireFiles(root, retired, current, Protected());
  Check(Has(removed, "rexgpu-xenos.dll") && !fs::exists(root / "rexgpu-xenos.dll"), "retire: unchanged DLL removed");
  Check(!fs::exists(root / "mods-builtin/ultrawide"), "retire: the emptied ultrawide folder removed");
  Check(fs::exists(root / "mods-builtin/hardcore/mod.toml"), "retire: the other built in mods stay");
  Check(fs::exists(root / "launcher/video/old.mp4"), "retire: a changed file is kept");
  Check(fs::exists(root / "launcher/video/kept.mp4"), "retire: a file this release ships is kept");
  Check(fs::exists(root / "game/old.bin"), "retire: protected kept");
  Check(fs::exists(root / "reshade.ini"), "retire: unlisted file kept");
  Check(!fs::exists(root / "olddir/a.txt") && fs::exists(root / "olddir/player.txt"),
        "retire: a folder with a player's file stays");
  Check(removed.size() == 4, "retire: exactly four removed");
}

void SourceTests(const fs::path& dir) {
  const fs::path src = dir / "dl", dst = dir / "inst";
  Put(src / "launcher/easter-egg/egg.exe", "egg");
  Put(src / "launcher/video/v.mp4", "re released");
  Put(dst / "king_kong.exe", "exe");
  const Manifest m = MakeList({{"king_kong.exe", "exe"}, {"launcher/easter-egg/egg.exe", "egg"},
                               {"launcher/video/v.mp4", "video"}});
  const auto needed = MissingOrWrong(dst, m, Protected(), kCheapHashLimit);
  Check(needed.size() == 2, "finish: two files needed");
  const auto r = InstallFromSource(src, dst, m, needed);
  Check(r.install.outcome == Outcome::kOk && r.installed.size() == 1 && Get(dst / "launcher/easter-egg/egg.exe") == "egg",
        "finish: the matching file installed");
  Check(r.unavailable.size() == 1 && !fs::exists(dst / "launcher/video/v.mp4"),
        "finish: a copy that does not match the list is not installed");
  Put(dst / "launcher/video/v.mp4", "video");
  Put(dst / "launcher/video/v.mp4.old", "left");
  Put(dst / "stray.txt", "x");
  RemoveOldCopies(dst, m);
  Check(!fs::exists(dst / "launcher/video/v.mp4.old"), "RemoveOldCopies");
  const auto extra = Unlisted(dst, m, Protected());
  Check(extra.size() == 1 && extra[0] == "stray.txt", "Unlisted finds the stray file only");
}

// codex\simulate-update.ps1's step: the new program's first start check on an install, from an unpacked release.
int Finish(const fs::path& install, const fs::path& source) {
  std::string error;
  const auto list = ReadManifest(install / kFilesList, &error);
  if (!list) {
    std::printf("NO kk-files.txt in the install after the old updater: %s\n", error.c_str());
    return 2;
  }
  const Protected keep;
  Manifest retired;
  if (const auto r = ReadManifest(install / kRetiredList, &error)) retired = *r;
  const auto removed = RetireFiles(install, retired, *list, keep);
  const auto needed = MissingOrWrong(install, *list, keep, kCheapHashLimit);
  uint64_t bytes = 0;
  for (const auto& p : needed)
    if (const auto* f = list->Find(p)) bytes += f->size;
  const auto r = InstallFromSource(source, install, *list, needed);
  std::printf("retired %zu\n", removed.size());
  for (const auto& p : removed) std::printf("  retired %s\n", p.c_str());
  std::printf("first start download needed: %s (%zu files, %.1f MB)\n", needed.empty() ? "no" : "YES",
              needed.size(), bytes / 1e6);
  for (size_t i = 0; i < needed.size(); ++i)
    if (i < 12) std::printf("  fetched %s\n", needed[i].c_str());
  if (needed.size() > 12) std::printf("  ... %zu more\n", needed.size() - 12);
  for (const auto& p : r.unavailable) std::printf("  UNAVAILABLE %s\n", p.c_str());
  const auto wrong = MissingOrWrong(install, *list, keep, UINT64_MAX);
  const auto stale = Unlisted(install, *list, keep);
  for (const auto& p : wrong) std::printf("  MISSING OR WRONG %s\n", p.c_str());
  for (const auto& p : stale) std::printf("  STALE %s\n", p.c_str());
  const bool ok = r.install.outcome == Outcome::kOk && wrong.empty() && stale.empty();
  std::printf("result %s: %zu missing or wrong, %zu stale (full hash of every file)\n", ok ? "MATCH" : "DIFFERS",
              wrong.size(), stale.size());
  return ok ? 0 : 1;
}

// codex\simulate-update.ps1's new updater step: what InstallUpdate does with an unpacked release that has a
// kk-files.txt (every listed file whose installed copy differs, kk-files.txt, the extra zip removed, retired
// files), then the first start check of the program it installed.
int Install(const fs::path& install, const fs::path& source) {
  std::string error;
  const auto list = ReadManifest(source / kFilesList, &error);
  if (!list) {
    std::printf("NO kk-files.txt in the release: %s\n", error.c_str());
    return 2;
  }
  const Protected keep;
  std::vector<std::string> plan = MissingOrWrong(install, *list, keep, UINT64_MAX);
  plan.push_back(kFilesList);
  const auto r = InstallFiles(source, install, plan);
  std::printf("new updater installed %zu files: %s\n", plan.size(), r.outcome == Outcome::kOk ? "ok" : "FAILED");
  if (r.outcome != Outcome::kOk) return 1;
  std::error_code ec;
  fs::remove(install / kExtraFilesZip, ec);
  Manifest retired;
  if (const auto rl = ReadManifest(source / kRetiredList, &error)) retired = *rl;
  const auto removed = RetireFiles(install, retired, *list, keep);
  std::printf("new updater retired %zu\n", removed.size());
  for (const auto& p : removed) std::printf("  retired %s\n", p.c_str());
  return Finish(install, source);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::string(argv[1]) == "--finish") return Finish(fs::path(argv[2]), fs::path(argv[3]));
  if (argc == 4 && std::string(argv[1]) == "--install") return Install(fs::path(argv[2]), fs::path(argv[3]));
  if (argc < 2) {
    std::printf("usage: kk-update-tests <scratch folder> | --finish <install> <release folder>\n");
    return 2;
  }
  const fs::path dir = fs::path(argv[1]);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);
  ParseTests();
  ProtectedTests();
  SumsTests();
  CheckTests(dir);
  InstallTests(dir);
  RetireTests(dir);
  SourceTests(dir);
  if (failures) {
    std::printf("%d update file checks FAILED\n", failures);
    return 1;
  }
  std::printf("update file checks: all ok\n");
  return 0;
}
