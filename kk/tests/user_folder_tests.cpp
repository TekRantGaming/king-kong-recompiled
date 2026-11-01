// The move of the player's data from Documents\king_kong to <program folder>\userdata (user_folder.h) on real files
// in a temp folder: copied and renamed, settings and installed.txt rewritten, a full disk, a move cut short at each
// stage, a locked file, data already in the program folder, a portable copy that must leave Documents alone.
//
// Usage: kk-user-folder-tests <scratch folder>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
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

#include "user_folder.h"

namespace {

namespace fs = std::filesystem;
using namespace kk::user_folder;

int failures = 0;
void Check(bool ok, const std::string& what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
  }
}

void Put(const fs::path& file, const std::string& text) {
  fs::create_directories(file.parent_path());
  std::ofstream(file, std::ios::binary) << text;
}

std::string Get(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string Slashes(const fs::path& p, const std::string& separator) {
  std::string s = p.string(), out;
  for (char c : s) {
    if (c == '\\' || c == '/') out += separator;
    else out += c;
  }
  return out;
}

struct Case {
  fs::path root, documents, program, user, toml;
  explicit Case(const fs::path& scratch, const char* name) : root(scratch / name) {
    std::error_code ec;
    fs::remove_all(root, ec);
    documents = root / "Documents" / "king_kong";
    program = root / "KingKong";
    user = program / kFolderName;
    toml = program / "king_kong.toml";
    fs::create_directories(program);
  }
  void Player() const {
    Put(documents / "B13EBABEBABEBABE" / "save.bin", std::string(5000, 's'));
    Put(documents / "cache" / "shaders.bin", std::string(200000, 'c'));
    Put(documents / "content" / "hd-textures" / "pack" / "pack.txt", "pack");
    Put(documents / "content" / "hd-textures" / "installed.txt",
        "name=HD\nfolder=" + Slashes(documents, "\\") + "\\content\\hd-textures\\pack\n");
    Put(documents / "mods" / "note.txt", "hello");
    fs::create_directories(documents / "empty");
    Put(toml, "[KK]\nnative_texture_pack_folder = \"" + Slashes(documents, "\\\\") +
                  "\\\\content\\\\hd-textures\\\\pack\"\nother = \"" + Slashes(documents, "/") + "/x\"\nkept = 1\n");
  }
  Options Make(bool rename) const {
    Options o;
    o.old_root = documents;
    o.new_root = user;
    o.config = toml;
    o.allow_rename = rename;
    o.log = [](const std::string& line) { (void)line; };
    return o;
  }
  void CheckMoved(const char* what) const {
    const std::string w = what;
    Check(Get(user / "B13EBABEBABEBABE" / "save.bin") == std::string(5000, 's'), w + ": save");
    Check(Get(user / "cache" / "shaders.bin") == std::string(200000, 'c'), w + ": cache");
    Check(Get(user / "mods" / "note.txt") == "hello", w + ": mods");
    Check(Get(user / "content" / "hd-textures" / "installed.txt") ==
              "name=HD\nfolder=" + Slashes(user, "\\") + "\\content\\hd-textures\\pack\n", w + ": installed.txt");
    const std::string settings = Get(toml);
    Check(settings.find(Slashes(user, "\\\\") + "\\\\content") != std::string::npos, w + ": settings, TOML spelling");
    Check(settings.find(Slashes(user, "/") + "/x") != std::string::npos, w + ": settings, / spelling");
    Check(settings.find("Documents") == std::string::npos, w + ": no old path left in the settings");
    Check(!fs::exists(documents), w + ": Documents folder gone");
    Check(fs::exists(user / kDone), w + ": done marker");
    Check(!fs::exists(user / kMoving) && !fs::exists(user / kCleaning), w + ": no other marker");
  }
};

void TestReplaceRoot() {
  Check(ReplaceRoot("a=C:\\D\\k\\x b=C:\\\\D\\\\k\\\\x c=C:/D/k/x d=C:\\D\\kong", "C:\\D\\k", "E:\\G\\u") ==
            "a=E:\\G\\u\\x b=E:\\\\G\\\\u\\\\x c=E:/G/u/x d=E:\\G\\uong", "ReplaceRoot spellings");
  Check(ReplaceRoot("nothing here", "C:\\D\\k", "E:\\u") == "nothing here", "ReplaceRoot no match");
}

void TestCopy(const fs::path& scratch) {
  Case c(scratch, "copy");
  c.Player();
  uint64_t last = 0, total = 0;
  Options o = c.Make(false);
  o.progress = [&](uint64_t done, uint64_t all) {
    last = done;
    total = all;
  };
  Check(Resolve(o) == c.user, "copy: the new folder");
  c.CheckMoved("copy");
  Check(total > 0 && last == total, "copy: progress reaches the end");
}

void TestRename(const fs::path& scratch) {
  Case c(scratch, "rename");
  c.Player();
  bool progress = false;
  Options o = c.Make(true);
  o.progress = [&](uint64_t, uint64_t) { progress = true; };
  Check(Resolve(o) == c.user, "rename: the new folder");
  c.CheckMoved("rename");
  Check(!progress, "rename: nothing copied");
}

void TestCloud(const fs::path& scratch) {
  Case c(scratch, "cloud");
  c.Player();
  bool progress = false;
  Options o = c.Make(true);
  o.cloud_file = [](const fs::path& file) { return file.filename() == "save.bin"; };
  o.progress = [&](uint64_t, uint64_t) { progress = true; };
  Check(Resolve(o) == c.user, "cloud: the new folder");
  c.CheckMoved("cloud");
  Check(progress, "cloud: a file that may be only in OneDrive means copying, not renaming");
}

void TestFresh(const fs::path& scratch) {
  Case c(scratch, "fresh");
  Check(Resolve(c.Make(true)) == c.user, "fresh: the new folder");
  Check(fs::exists(c.user / kDone), "fresh: done, Documents is never read again");
  // A portable copy on a PC that has an old Documents folder.
  c.Player();
  Check(Resolve(c.Make(true)) == c.user, "portable: the new folder");
  Check(fs::exists(c.documents / "B13EBABEBABEBABE" / "save.bin"), "portable: that PC's Documents left alone");
  Check(!fs::exists(c.user / "B13EBABEBABEBABE"), "portable: nothing taken from it");
}

void TestFull(const fs::path& scratch) {
  Case c(scratch, "full");
  c.Player();
  Options o = c.Make(false);
  o.free_space = [](const fs::path&) { return uint64_t(1000); };
  Check(Resolve(o) == c.documents, "full disk: stays in the old folder");
  Check(Get(c.documents / "B13EBABEBABEBABE" / "save.bin") == std::string(5000, 's'), "full disk: save untouched");
  Check(Get(c.toml).find("Documents") != std::string::npos, "full disk: settings untouched");
  // The player makes room: the next start moves it.
  o.free_space = nullptr;
  Check(Resolve(o) == c.user, "full disk then room: the new folder");
  c.CheckMoved("full disk then room");
}

// A save the game wrote while the move had failed must win over the partial copy.
void TestResumeNewer(const fs::path& scratch) {
  Case c(scratch, "resume");
  c.Player();
  Put(c.user / kMoving, "Moving\n");
  Put(c.user / "B13EBABEBABEBABE" / "save.bin", "partial");
  Put(c.user / "cache" / "shaders.bin.moving", "partial");
  Check(Resolve(c.Make(false)) == c.user, "resume: the new folder");
  c.CheckMoved("resume");
}

void TestResumeAfterRenames(const fs::path& scratch) {
  Case c(scratch, "renamed");
  c.Player();
  fs::create_directories(c.user);
  Put(c.user / kMoving, "Moving\n");
  fs::rename(c.documents / "content", c.user / "content");  // cut after this rename
  Check(Resolve(c.Make(true)) == c.user, "cut in the renames: the new folder");
  c.CheckMoved("cut in the renames");
}

void TestResumeOldGone(const fs::path& scratch) {
  Case c(scratch, "gone");
  c.Player();
  fs::create_directories(c.user);
  Put(c.user / kMoving, "Moving\n");
  for (const char* name : {"B13EBABEBABEBABE", "cache", "content", "mods", "empty"})
    fs::rename(c.documents / name, c.user / name);
  fs::remove_all(c.documents);  // cut after the last rename, before the settings
  Check(Resolve(c.Make(true)) == c.user, "cut after the renames: the new folder");
  c.CheckMoved("cut after the renames");
}

void TestOwnData(const fs::path& scratch) {
  Case c(scratch, "own");
  c.Player();
  Put(c.user / "mods" / "note.txt", "newer");
  Options o = c.Make(false);
  Check(Resolve(o) == c.user, "own data: the new folder");
  Check(Get(c.user / "mods" / "note.txt") == "newer", "own data: never overwritten");
  Check(Get(c.user / "B13EBABEBABEBABE" / "save.bin") == std::string(5000, 's'), "own data: the rest is moved");
  Check(Get(c.documents / "mods" / "note.txt") == "hello", "own data: the old file that differs stays");
  Check(!fs::exists(c.documents / "B13EBABEBABEBABE"), "own data: moved files gone from the old folder");
  Check(fs::exists(c.user / kDone), "own data: done");
}

void TestInsideOld(const fs::path& scratch) {
  Case c(scratch, "inside");
  c.Player();
  Options o = c.Make(false);
  o.new_root = c.documents / kFolderName;
  Check(Resolve(o) == c.documents, "game put inside the old folder: stays");
  Check(fs::exists(c.documents / "B13EBABEBABEBABE" / "save.bin"), "game put inside the old folder: nothing moved");
  Check(!fs::exists(o.new_root), "game put inside the old folder: no new folder");
}

void TestNotWritable(const fs::path& scratch) {
  Case c(scratch, "readonly");
  c.Player();
  Options o = c.Make(false);
  Put(c.root / "file", "a file");
  o.new_root = c.root / "file" / kFolderName;  // cannot be made
  Check(Resolve(o) == c.documents, "program folder not writable: stays");
  Check(fs::exists(c.documents / "B13EBABEBABEBABE" / "save.bin"), "program folder not writable: nothing moved");
}

#if defined(_WIN32)
// A file another program holds: the copy fails, renames are put back, the old folder still has everything; the
// next start finishes. Then a file held while the old folder is cleaned stays until a later start.
void TestLocked(const fs::path& scratch) {
  Case c(scratch, "locked");
  c.Player();
  const fs::path held = c.documents / "mods" / "note.txt";
  HANDLE file = CreateFileW(held.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  Check(file != INVALID_HANDLE_VALUE, "locked: test file open");
  Check(Resolve(c.Make(true)) == c.documents, "locked: stays in the old folder");
  Check(fs::exists(c.documents / "B13EBABEBABEBABE" / "save.bin"), "locked: saves put back");
  Check(fs::exists(c.documents / "content" / "hd-textures" / "installed.txt"), "locked: content put back");
  Check(Get(c.toml).find("Documents") != std::string::npos, "locked: settings untouched");
  CloseHandle(file);
  Check(Resolve(c.Make(true)) == c.user, "locked then free: the new folder");
  c.CheckMoved("locked then free");
}

void TestLockedClean(const fs::path& scratch) {
  Case c(scratch, "lockclean");
  c.Player();
  const fs::path held = c.documents / "mods" / "note.txt";
  HANDLE file = CreateFileW(held.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  Check(Resolve(c.Make(false)) == c.user, "held in the clean up: the new folder");
  Check(fs::exists(c.user / kCleaning), "held in the clean up: still cleaning");
  Check(fs::exists(held), "held in the clean up: file stays");
  CloseHandle(file);
  Check(Resolve(c.Make(false)) == c.user, "held then free: the new folder");
  c.CheckMoved("held then free");
}
#endif

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: kk-user-folder-tests <scratch folder>\n");
    return 2;
  }
  const fs::path scratch = fs::absolute(argv[1]);
  fs::create_directories(scratch);
  TestReplaceRoot();
  TestCopy(scratch);
  TestRename(scratch);
  TestCloud(scratch);
  TestFresh(scratch);
  TestFull(scratch);
  TestResumeNewer(scratch);
  TestResumeAfterRenames(scratch);
  TestResumeOldGone(scratch);
  TestOwnData(scratch);
  TestInsideOld(scratch);
  TestNotWritable(scratch);
#if defined(_WIN32)
  TestLocked(scratch);
  TestLockedClean(scratch);
#endif
  if (failures == 0) std::printf("user folder tests passed\n");
  return failures == 0 ? 0 : 1;
}
