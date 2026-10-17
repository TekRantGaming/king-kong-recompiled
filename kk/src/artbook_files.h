// Beta 7 Extras, the Weta Artbook: the files part (artbook_logic.h has the pure parts). Turns the downloaded bonus
// disc (.bin, raw MODE2/2352 sectors) into an ISO in a streaming way, takes the six paintings and the readme's
// text out of it with our own ISO 9660 reader, checks each picture, and copies a picture into the player's
// Pictures folder (Save to PC). Used by the content service's worker (content.cpp, Kind::kArtbook) and by the
// Extras > Artbook page (artbook.cpp); no SDK, so the tests build it on its own.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace kk::artbook {

// Called with bytes done so far; returning false stops (the function then fails with "stopped").
using Progress = std::function<bool(uint64_t done)>;
// Whether a picture file decodes (the launcher passes a real decoder; the default checks the JPEG header).
using PictureCheck = std::function<bool(const std::filesystem::path& file)>;

// Raw CD image (2352-byte sectors, Mode 1 or Mode 2 Form 1) to its 2048-byte user data, read and written in
// blocks of about 1 MB. False (and `why`) on a read or write error, a size that is not whole sectors, or a
// sector that is not data.
bool ConvertToIso(const std::filesystem::path& raw, const std::filesystem::path& iso, const Progress& progress,
                  std::string& why);

struct Extracted {
  std::vector<std::filesystem::path> pictures;  // in `out`
  bool readme = false;                          // readme.txt written
};
// From the ISO: every .jpg in BonusMaterials\Collector's Art\Weta Collector's Artwork and the readme's text
// (Readme\*.doc as readme.txt) into the existing, empty folder `out`. Each picture must pass `check`. Nothing
// else on the disc is read (not the screensaver installer). False (and `why`) when the folder or a picture is
// missing or a picture does not decode; the readme is optional.
bool ExtractArtbook(const std::filesystem::path& iso, const std::filesystem::path& out, const PictureCheck& check,
                    const Progress& progress, Extracted& result, std::string& why);

// A whole file (at most `limit` bytes; false when bigger or unreadable).
bool ReadBytes(const std::filesystem::path& file, std::vector<uint8_t>& out, uint64_t limit);
// The first bytes of a file (up to `n`).
std::vector<uint8_t> ReadHead(const std::filesystem::path& file, size_t n);

// The default picture check: a JPEG with a frame header of a sensible size.
bool JpegHeaderOk(const std::filesystem::path& file);

// The installed paintings in `folder` (its .jpg files, sorted by name) and the readme's text ("" if none).
std::vector<std::filesystem::path> Paintings(const std::filesystem::path& folder);
std::string ReadmeText(const std::filesystem::path& folder);
bool Installed(const std::filesystem::path& folder);

// The player's Pictures folder: Windows' known Pictures folder (it follows a move to OneDrive or another drive),
// Linux's XDG pictures folder (user-dirs.dirs, else ~/Pictures). Developer builds: KK_DEV_ARTBOOK_PICTURES
// replaces it (tests never write into the real one).
std::filesystem::path PicturesDir();
// PicturesDir() \ "King Kong Artbook".
std::filesystem::path SaveFolder();

struct Saved {
  bool ok = false;
  bool already = false;          // the same picture was already there (nothing copied)
  std::filesystem::path file;    // where it is
  std::string why;               // when not ok
};
// Copies `picture` into `folder` (made if missing) under its own name, or "name (2).jpg" and so on when a
// different file has that name.
Saved SavePicture(const std::filesystem::path& picture, const std::filesystem::path& folder);

// The automatic background download (launcher.cpp TickArtbook): wanted since the end of the setup or an update,
// failed tries in a row, and whether the player was told. In <user data>\content\downloads\artbook.auto until
// the artbook is installed.
struct AutoState {
  bool wanted = false;
  int fails = 0;
  bool told = false;
};
std::filesystem::path AutoFile(const std::filesystem::path& user_dir);
AutoState ReadAuto(const std::filesystem::path& user_dir);
bool WriteAuto(const std::filesystem::path& user_dir, const AutoState& state);
void ClearAuto(const std::filesystem::path& user_dir);

}  // namespace kk::artbook
