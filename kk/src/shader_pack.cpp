#include "shader_pack.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "http.h"
#include "platform.h"

#include <ctime>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <rex/hash.h>
#include <rex/logging.h>

namespace kk {
namespace {

std::vector<uint8_t> ReadAll(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

uint64_t Load64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}
uint32_t Load32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

// Storage files written by the runtime (host byte order):
//   .xsh:  'XESH', version, then per shader {u64 hash, u32 dword count | type << 31, ucode}
//   .xpso: 'XEPS', 'DXRT'/'DXRO', version, then 72-byte {u64 hash, description} records
constexpr uint32_t kShaderMagic = 0x48534558, kPipelineMagic = 0x53504558;
constexpr size_t kPipelineRecord = 72;

// Each record starts with the XXH3 of the rest of it (a shader's microcode, or
// a pipeline's description). The runtime stops reading a file at the first
// record that doesn't match, so one record damaged by a write the game never
// finished (it was closed or killed mid-save) hides every record after it,
// including a shader pack merged in later. Parse keeps only whole, matching
// records and counts the rest.
struct Records {
  size_t header = 0;
  std::vector<std::pair<size_t, size_t>> spans;  // offset, size
  size_t damaged = 0;                            // records left out
};

bool Parse(const std::vector<uint8_t>& d, Records& out) {
  if (d.size() < 8) return false;
  const uint32_t magic = Load32(d.data());
  if (magic == kShaderMagic) {
    out.header = 8;
    for (size_t at = 8; at + 12 <= d.size();) {
      const size_t size = 12 + size_t(Load32(&d[at + 8]) & 0x7FFFFFFF) * 4;
      if (at + size > d.size()) {  // cut short: keep what is whole
        ++out.damaged;
        break;
      }
      if (XXH3_64bits(&d[at + 12], size - 12) == Load64(&d[at])) out.spans.push_back({at, size});
      else ++out.damaged;
      at += size;
    }
    return true;
  }
  if (magic == kPipelineMagic && d.size() >= 12) {
    out.header = 12;
    for (size_t at = 12; at + kPipelineRecord <= d.size(); at += kPipelineRecord) {
      if (XXH3_64bits(&d[at + 8], kPipelineRecord - 8) == Load64(&d[at])) out.spans.push_back({at, kPipelineRecord});
      else ++out.damaged;
    }
    if ((d.size() - 12) % kPipelineRecord) ++out.damaged;  // a partly written last record
    return true;
  }
  return false;
}

// Writes the header and the given records (whole records only).
bool WriteRecords(const std::filesystem::path& path, const std::vector<uint8_t>& d, const Records& r,
                  const std::vector<uint8_t>* extra = nullptr) {
  std::vector<uint8_t> out(d.begin(), d.begin() + r.header);
  for (auto [at, size] : r.spans) out.insert(out.end(), d.begin() + at, d.begin() + at + size);
  if (extra) out.insert(out.end(), extra->begin(), extra->end());
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
  return bool(f);
}

bool Fetch(const std::string& url, std::vector<uint8_t>& out, ShaderPackStatus* status) {
  return kk::HttpGet(url, out, status ? &status->bytes : nullptr, status ? &status->total : nullptr);
}

struct Manifest {
  int version = 0;
  std::vector<std::string> files;
};

Manifest ParseManifest(const std::string& text) {
  Manifest m;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.rfind("version=", 0) == 0) m.version = std::atoi(line.c_str() + 8);
    else if (!line.empty() && line[0] != '#' && line.find('/') == std::string::npos &&
             line.find('\\') == std::string::npos && line.find("..") == std::string::npos)
      m.files.push_back(line);
  }
  return m;
}

}  // namespace

int MergeShaderStorageFile(const std::filesystem::path& from, const std::filesystem::path& into) {
  const auto src = ReadAll(from);
  Records src_records;
  if (!Parse(src, src_records)) return -1;
  std::error_code ec;
  if (!std::filesystem::exists(into, ec) || std::filesystem::file_size(into, ec) <= src_records.header) {
    std::filesystem::create_directories(into.parent_path(), ec);
    std::filesystem::copy_file(from, into, std::filesystem::copy_options::overwrite_existing, ec);
    return ec ? -1 : int(src_records.spans.size());
  }
  auto dst = ReadAll(into);
  Records dst_records;
  if (!Parse(dst, dst_records) || dst_records.header != src_records.header ||
      std::memcmp(dst.data(), src.data(), src_records.header) != 0)
    return -1;  // another runtime version: leave the player's cache alone
  // Keep the player's whole records (dropping any damaged ones), then append what is missing.
  std::unordered_set<uint64_t> have;
  for (auto [at, size] : dst_records.spans) have.insert(Load64(&dst[at]));
  std::vector<uint8_t> extra;
  int added = 0;
  for (auto [at, size] : src_records.spans) {
    if (!have.insert(Load64(&src[at])).second) continue;
    extra.insert(extra.end(), src.begin() + at, src.begin() + at + size);
    ++added;
  }
  if (dst_records.damaged)
    REXLOG_WARN("KK: shader cache {}: dropped {} damaged record(s)", into.filename().string(), dst_records.damaged);
  if ((added || dst_records.damaged) && !WriteRecords(into, dst, dst_records, &extra)) return -1;
  return added;
}

int RepairShaderStorage(const std::filesystem::path& cache_dir) {
  namespace fs = std::filesystem;
  std::error_code ec;
  int dropped = 0;
  for (const char* sub : {"shareable", "local"}) {
    const fs::path dir = cache_dir / "shaders" / sub;
    if (!fs::is_directory(dir, ec)) continue;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      const auto ext = entry.path().extension().string();
      if (ext != ".xsh" && ext != ".xpso") continue;
      const auto data = ReadAll(entry.path());
      Records r;
      if (!Parse(data, r) || !r.damaged) continue;
      if (WriteRecords(entry.path(), data, r)) {
        REXLOG_WARN("KK: shader cache {}: dropped {} damaged record(s), kept {}", entry.path().filename().string(),
                    r.damaged, r.spans.size());
        dropped += int(r.damaged);
      }
    }
  }
  return dropped;
}

std::filesystem::path PackShadersForSharing(const std::filesystem::path& cache_dir,
                                            const std::filesystem::path& user_dir, std::string& error) {
  namespace fs = std::filesystem;
  const fs::path shareable = cache_dir / "shaders" / "shareable";
  std::error_code ec;
  std::wstring files;
  uint64_t size = 0;
  for (auto& e : fs::directory_iterator(shareable, ec)) {
    const auto ext = e.path().extension();
    if (!e.is_regular_file() || (ext != ".xsh" && ext != ".xpso")) continue;  // shader data only
    files += L" \"" + e.path().filename().wstring() + L"\"";
    size += e.file_size(ec);
  }
  if (files.empty()) {
    error = "There are no shaders to share yet. Play the game for a while first.";
    return {};
  }
  if (size > 24ull << 20) {
    error = "Your shaders are too big for GitHub (over 25 MB).";
    return {};
  }
  char name[64];
  const std::time_t now = std::time(nullptr);
  std::strftime(name, sizeof(name), "shader-share-%Y%m%d-%H%M%S.zip", std::localtime(&now));
  const fs::path zip = user_dir / name;
#if defined(_WIN32)
  // Windows 10 and 11 include tar, which writes zip files with -a.
  wchar_t system_dir[260];
  GetSystemDirectoryW(system_dir, 260);
  const std::wstring tar = (std::filesystem::path(system_dir) / "tar.exe").wstring();
  const std::wstring cmd =
      L"\"" + tar + L"\" -a -c -f \"" + zip.wstring() + L"\" -C \"" + shareable.wstring() + L"\"" + files;
  if (!RunAndWait(cmd) || !fs::exists(zip, ec)) {
    error = "Could not create the zip file.";
    return {};
  }
  REXLOG_INFO("KK: packed shaders for sharing: {}", zip.string());
  return zip;
#else
  error = "Sharing shaders is only available on Windows for now.";
  return {};
#endif
}

int InstalledShaderPackVersion(const std::filesystem::path& cache_dir) {
  std::ifstream f(cache_dir / "shaders" / "shader-pack.txt");
  std::stringstream text;
  text << f.rdbuf();
  return ParseManifest(text.str()).version;
}

void DownloadAndInstallShaderPack(const std::filesystem::path& cache_dir, ShaderPackStatus& status) {
  status.busy = true;
  status.bytes = 0;
  status.total = 0;
  auto fail = [&](std::string why) {
    REXLOG_WARN("KK: shader pack: {}", why);
    status.message = std::move(why);
    status.failed = true;
    status.busy = false;
  };
  std::vector<uint8_t> manifest_data;
  if (!Fetch(std::string(kShaderPackUrl) + "shader-pack.txt", manifest_data, nullptr))
    return fail("Could not reach GitHub. Check your internet connection and try again.");
  const std::string manifest_text(manifest_data.begin(), manifest_data.end());
  const Manifest manifest = ParseManifest(manifest_text);
  if (!manifest.version || manifest.files.empty()) return fail("The shader pack on GitHub looks incomplete.");
  if (const int installed = InstalledShaderPackVersion(cache_dir); installed >= manifest.version) {
    status.message = "You already have the newest shader pack (pack " + std::to_string(installed) + ").";
    status.done = true;
    status.busy = false;
    return;
  }

  const auto shareable = cache_dir / "shaders" / "shareable";
  const auto download_dir = cache_dir / "shaders" / "download";
  std::error_code ec;
  std::filesystem::create_directories(download_dir, ec);
  int added = 0;
  for (const auto& name : manifest.files) {
    std::vector<uint8_t> data;
    if (!Fetch(std::string(kShaderPackUrl) + name, data, &status)) return fail("Downloading " + name + " failed.");
    const auto tmp = download_dir / name;
    std::ofstream(tmp, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    const int n = MergeShaderStorageFile(tmp, shareable / name);
    std::filesystem::remove(tmp, ec);
    if (n < 0) return fail("The shader pack is for a different version of the game port. Update the port first.");
    added += n;
  }
  std::filesystem::remove(download_dir, ec);
  std::ofstream(cache_dir / "shaders" / "shader-pack.txt") << manifest_text;
  status.message = "Shader pack " + std::to_string(manifest.version) +
                   " installed. The game prepares its effects each time it starts.";
  REXLOG_INFO("KK: shader pack added {} records", added);
  REXLOG_INFO("KK: {}", status.message);
  status.done = true;
  status.busy = false;
}

}  // namespace kk
