#include "shader_pack.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

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

struct Records {
  size_t header = 0;
  std::vector<std::pair<size_t, size_t>> spans;  // offset, size
};

bool Parse(const std::vector<uint8_t>& d, Records& out) {
  if (d.size() < 8) return false;
  const uint32_t magic = Load32(d.data());
  if (magic == kShaderMagic) {
    out.header = 8;
    for (size_t at = 8; at + 12 <= d.size();) {
      const size_t size = 12 + size_t(Load32(&d[at + 8]) & 0x7FFFFFFF) * 4;
      if (at + size > d.size()) break;  // cut short: keep what is whole
      out.spans.push_back({at, size});
      at += size;
    }
    return true;
  }
  if (magic == kPipelineMagic && d.size() >= 12) {
    out.header = 12;
    for (size_t at = 12; at + kPipelineRecord <= d.size(); at += kPipelineRecord) out.spans.push_back({at, kPipelineRecord});
    return true;
  }
  return false;
}

#if defined(_WIN32)
// HTTPS GET into memory (follows GitHub's redirect to its download host).
bool HttpGet(const std::string& url, std::vector<uint8_t>& out, ShaderPackStatus* status) {
  URL_COMPONENTSW parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256], path[2048];
  parts.lpszHostName = host;
  parts.dwHostNameLength = 256;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 2048;
  const std::wstring wurl(url.begin(), url.end());
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) return false;
  HINTERNET session = WinHttpOpen(L"KingKongRecomp/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;
  bool ok = false;
  if (HINTERNET conn = WinHttpConnect(session, host, parts.nPort, 0)) {
    if (HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {
      DWORD code = 0, len = sizeof(code);
      if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
          WinHttpReceiveResponse(req, nullptr) &&
          WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &len,
                              nullptr) &&
          code == 200) {
        DWORD content = 0;
        len = sizeof(content);
        if (status && WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, nullptr,
                                          &content, &len, nullptr))
          status->total += content;
        ok = true;
        for (DWORD avail = 0; WinHttpQueryDataAvailable(req, &avail) && avail;) {
          const size_t at = out.size();
          out.resize(at + avail);
          DWORD read = 0;
          if (!WinHttpReadData(req, out.data() + at, avail, &read)) {
            ok = false;
            break;
          }
          out.resize(at + read);
          if (status) status->bytes += read;
        }
      }
      WinHttpCloseHandle(req);
    }
    WinHttpCloseHandle(conn);
  }
  WinHttpCloseHandle(session);
  return ok;
}
#else
bool HttpGet(const std::string&, std::vector<uint8_t>&, ShaderPackStatus*) { return false; }
#endif

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
  // Drop a partly written last record, then append what is missing.
  size_t end = dst_records.header;
  std::unordered_set<uint64_t> have;
  for (auto [at, size] : dst_records.spans) {
    have.insert(Load64(&dst[at]));
    end = at + size;
  }
  dst.resize(end);
  int added = 0;
  for (auto [at, size] : src_records.spans) {
    if (!have.insert(Load64(&src[at])).second) continue;
    dst.insert(dst.end(), src.begin() + at, src.begin() + at + size);
    ++added;
  }
  if (added) {
    std::ofstream out(into, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(dst.data()), std::streamsize(dst.size()));
    if (!out) return -1;
  }
  return added;
}

int InstalledShaderPackVersion(const std::filesystem::path& cache_dir) {
  std::ifstream f(cache_dir / "shaders" / "shader-pack.txt");
  std::stringstream text;
  text << f.rdbuf();
  return ParseManifest(text.str()).version;
}

int FetchShaderPackVersion() {
  std::vector<uint8_t> data;
  if (!HttpGet(std::string(kShaderPackUrl) + "shader-pack.txt", data, nullptr)) return 0;
  return ParseManifest(std::string(data.begin(), data.end())).version;
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
  if (!HttpGet(std::string(kShaderPackUrl) + "shader-pack.txt", manifest_data, nullptr))
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
    if (!HttpGet(std::string(kShaderPackUrl) + name, data, &status)) return fail("Downloading " + name + " failed.");
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
