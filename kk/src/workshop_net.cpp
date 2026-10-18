#include "workshop_net.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <winhttp.h>
#include <wrl/client.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#else
#include "platform.h"
// stb_image (public domain / MIT, the same copy the ReXGlue SDK carries), PNG, JPEG and GIF only, all of
// it internal to this file.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_NO_FAILURE_STRINGS
#define STBI_MAX_DIMENSIONS 8192
#include "../third_party/stb/stb_image.h"
#endif

#ifndef KK_VERSION
#define KK_VERSION "dev"
#endif
#ifndef KK_VERSION_W
#define KK_VERSION_W L"dev"
#endif

namespace kk::workshop {

namespace fs = std::filesystem;

// ------------------------------------------------------------------ fetch ---
#if defined(_WIN32)
namespace {

struct Handle {
  HINTERNET h = nullptr;
  ~Handle() {
    if (h) WinHttpCloseHandle(h);
  }
};

std::wstring Wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::string Header(HINTERNET req, DWORD query) {
  DWORD len = 0;
  WinHttpQueryHeaders(req, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &len, WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0 || len > 8192) return {};
  std::wstring buf(len / sizeof(wchar_t) + 1, L'\0');
  if (!WinHttpQueryHeaders(req, query, WINHTTP_HEADER_NAME_BY_INDEX, buf.data(), &len, WINHTTP_NO_HEADER_INDEX))
    return {};
  std::string out;
  for (DWORD i = 0; i < len / sizeof(wchar_t); ++i) out.push_back(buf[i] < 128 ? char(buf[i]) : '?');
  return out;
}

}  // namespace

FetchReply HttpFetch(const FetchRequest& request, const std::atomic<bool>* stop) {
  FetchReply r;
  if (!StartsWith(request.url, "https://")) return r;
  const std::wstring wurl = Wide(request.url);
  URL_COMPONENTSW parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256], path[4096];
  parts.lpszHostName = host;
  parts.dwHostNameLength = 256;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 4096;
  wchar_t extra[2048];
  parts.lpszExtraInfo = extra;
  parts.dwExtraInfoLength = 2048;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) return r;
  const std::wstring object = std::wstring(path, parts.dwUrlPathLength) + std::wstring(extra, parts.dwExtraInfoLength);
  Handle session{WinHttpOpen(L"KingKongRecomp/" KK_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session.h) return r;
  // Name lookup, connect, send, each read: a stalled server gives up within seconds.
  WinHttpSetTimeouts(session.h, 8000, 8000, 10000, 15000);
  Handle conn{WinHttpConnect(session.h, host, parts.nPort, 0)};
  if (!conn.h) return r;
  Handle req{WinHttpOpenRequest(conn.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
  if (!req.h) return r;
  std::wstring headers;
  for (const auto& [k, v] : request.headers) headers += Wide(k) + L": " + Wide(v) + L"\r\n";
  DWORD status = 0, len = sizeof(status);
  if (!WinHttpSendRequest(req.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                          headers.empty() ? 0 : DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(req.h, nullptr) ||
      !WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &len,
                           nullptr))
    return r;
  r.etag = Header(req.h, WINHTTP_QUERY_ETAG);
  r.last_modified = Header(req.h, WINHTTP_QUERY_LAST_MODIFIED);
  if (status == 200 || status == 206) {  // 206: a ranged request (task 221)
    for (DWORD avail = 0; WinHttpQueryDataAvailable(req.h, &avail) && avail;) {
      if ((stop && stop->load()) || r.body.size() + avail > request.max_bytes) {
        r.body.clear();
        return FetchReply{};
      }
      const size_t at = r.body.size();
      r.body.resize(at + avail);
      DWORD read = 0;
      if (!WinHttpReadData(req.h, r.body.data() + at, avail, &read)) return FetchReply{};
      r.body.resize(at + read);
    }
  }
  r.status = int(status);
  return r;
}
#else
FetchReply HttpFetch(const FetchRequest& request, const std::atomic<bool>* stop) {
  FetchReply r;
  if (!StartsWith(request.url, "https://") || (stop && stop->load())) return r;
  // The status and validators follow the body, after a marker line (curl 7.84 or later expands %header;
  // an older curl prints the names, which count as none).
  static const std::string kMarker = "\n@@KK-WORKSHOP@@";
  std::vector<std::string> argv = {"curl", "-sS", "-L", "--proto", "=https", "--proto-redir", "=https",
                                   "--max-redirs", "5", "--connect-timeout", "10", "--max-time", "25",
                                   "--max-filesize", std::to_string(request.max_bytes), "-A",
                                   std::string("KingKongRecomp/") + KK_VERSION,
                                   "-w", kMarker + "%{http_code}\n%header{etag}\n%header{last-modified}"};
  for (const auto& [k, v] : request.headers) {
    argv.push_back("-H");
    argv.push_back(k + ": " + v);
  }
  argv.push_back(request.url);
  std::vector<uint8_t> out;
  if (!RunCapture(argv, out)) return r;
  const std::string text(out.begin(), out.end());
  const size_t at = text.rfind(kMarker);
  if (at == std::string::npos) return r;
  std::vector<std::string> fields;
  size_t start = at + kMarker.size();
  for (;;) {
    const size_t nl = text.find('\n', start);
    fields.push_back(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  auto field = [&](size_t i) {
    std::string v = i < fields.size() ? fields[i] : std::string();
    if (v.rfind("%header", 0) == 0) v.clear();
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
    return v;
  };
  r.status = std::atoi(field(0).c_str());
  r.etag = field(1);
  r.last_modified = field(2);
  if (r.status == 200 || r.status == 206) {  // 206: a ranged request (task 221)
    if (size_t(at) > request.max_bytes) return FetchReply{};
    r.body.assign(out.begin(), out.begin() + std::ptrdiff_t(at));
  }
  return r;
}
#endif

// ------------------------------------------------------------------ cache ---
namespace {

bool ReadAll(const fs::path& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

bool WriteAtomically(const fs::path& path, const void* data, size_t size) {
  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(static_cast<const char*>(data), std::streamsize(size));
    if (!f) return false;
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);  // replaces the old copy
  return !ec;
}

}  // namespace

CacheResult CachedGet(const fs::path& dir, const std::string& url, const Headers& headers, size_t max_bytes,
                      int64_t now, int64_t max_age, const Fetcher& fetch) {
  CacheResult result;
  const std::string key = CacheKey(url);
  const fs::path bin = dir / (key + ".bin"), meta_path = dir / (key + ".meta");
  CacheMeta meta;
  std::vector<uint8_t> cached;
  bool have = false;
  {
    std::vector<uint8_t> meta_bytes;
    if (ReadAll(meta_path, meta_bytes) && ParseMeta(std::string(meta_bytes.begin(), meta_bytes.end()), meta) &&
        meta.url == url && ReadAll(bin, cached))
      have = true;
  }
  if (have && (!fetch || IsFresh(meta, now, max_age))) {
    result.ok = true;
    result.from_cache = true;
    result.bytes = std::move(cached);
    return result;
  }
  if (!fetch) return result;
  FetchRequest request{url, headers, max_bytes};
  if (have)
    for (auto& h : ConditionalHeaders(meta)) request.headers.push_back(std::move(h));
  FetchReply reply = fetch(request);
  result.asked = true;
  result.status = reply.status;
  switch (DecideCacheStep(have, reply.status)) {
    case CacheStep::kStore: {
      std::error_code ec;
      fs::create_directories(dir, ec);
      CacheMeta fresh{url, reply.etag, reply.last_modified, now};
      const std::string text = FormatMeta(fresh);
      // The body first: a meta file always names a whole body.
      if (WriteAtomically(bin, reply.body.data(), reply.body.size())) WriteAtomically(meta_path, text.data(), text.size());
      result.ok = true;
      result.bytes = std::move(reply.body);
      return result;
    }
    case CacheStep::kKeep: {
      meta.fetched = now;
      if (!reply.etag.empty()) meta.etag = reply.etag;
      if (!reply.last_modified.empty()) meta.last_modified = reply.last_modified;
      const std::string text = FormatMeta(meta);
      WriteAtomically(meta_path, text.data(), text.size());
      result.ok = true;
      result.from_cache = true;
      result.bytes = std::move(cached);
      return result;
    }
    case CacheStep::kUseStale:
      result.ok = true;
      result.from_cache = true;
      result.bytes = std::move(cached);
      return result;
    case CacheStep::kFail:
      return result;
  }
  return result;
}

// --------------------------------------------------------------- pictures ---
bool LooksLikePicture(const std::vector<uint8_t>& b) {
  if (b.size() >= 8 && std::memcmp(b.data(), "\x89PNG\r\n\x1a\n", 8) == 0) return true;
  if (b.size() >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return true;
  return b.size() >= 6 && (std::memcmp(b.data(), "GIF87a", 6) == 0 || std::memcmp(b.data(), "GIF89a", 6) == 0);
}

#if defined(_WIN32)
Picture DecodePicture(const std::vector<uint8_t>& bytes, int max_width) {
  using Microsoft::WRL::ComPtr;
  Picture pic;
  if (!LooksLikePicture(bytes) || bytes.size() > 0xFFFFFFFFu) return pic;
  // This thread's COM (the Workshop's worker; harmless when it is already initialised).
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  struct ComEnd {
    bool on;
    ~ComEnd() {
      if (on) CoUninitialize();
    }
  } com_end{SUCCEEDED(com)};
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
      FAILED(factory->CreateStream(&stream)) ||
      FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), DWORD(bytes.size()))) ||
      FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame)))
    return pic;
  UINT w = 0, h = 0;
  if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0 || w > 8192 || h > 8192) return pic;
  ComPtr<IWICBitmapSource> source = frame;
  UINT ow = w, oh = h;
  if (max_width > 0 && int(w) > max_width) {
    ow = UINT(max_width);
    oh = std::max<UINT>(1, UINT(uint64_t(h) * ow / w));
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), ow, oh, WICBitmapInterpolationModeFant)))
      return pic;
    source = scaler;
  }
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)))
    return pic;
  std::vector<uint8_t> rgba(size_t(ow) * oh * 4);
  if (FAILED(converter->CopyPixels(nullptr, ow * 4, UINT(rgba.size()), rgba.data()))) return pic;
  pic.width = int(ow);
  pic.height = int(oh);
  pic.rgba = std::move(rgba);
  return pic;
}
#else
Picture DecodePicture(const std::vector<uint8_t>& bytes, int max_width) {
  Picture pic;
  if (!LooksLikePicture(bytes) || bytes.size() > 0x7FFFFFFF) return pic;
  int w = 0, h = 0, channels = 0;
  stbi_uc* px = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels, 4);
  if (!px) return pic;
  if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
    stbi_image_free(px);
    return pic;
  }
  if (max_width > 0 && w > max_width) {
    // A box filter: each output pixel averages the source pixels it covers.
    const int ow = max_width, oh = std::max(1, int(int64_t(h) * ow / w));
    pic.rgba.assign(size_t(ow) * oh * 4, 0);
    for (int y = 0; y < oh; ++y) {
      const int y0 = int(int64_t(y) * h / oh), y1 = std::max(y0 + 1, int(int64_t(y + 1) * h / oh));
      for (int x = 0; x < ow; ++x) {
        const int x0 = int(int64_t(x) * w / ow), x1 = std::max(x0 + 1, int(int64_t(x + 1) * w / ow));
        uint32_t sum[4] = {0, 0, 0, 0};
        for (int sy = y0; sy < y1; ++sy)
          for (int sx = x0; sx < x1; ++sx)
            for (int c = 0; c < 4; ++c) sum[c] += px[(size_t(sy) * w + sx) * 4 + c];
        const uint32_t n = uint32_t((y1 - y0) * (x1 - x0));
        for (int c = 0; c < 4; ++c) pic.rgba[(size_t(y) * ow + x) * 4 + c] = uint8_t(sum[c] / n);
      }
    }
    pic.width = ow;
    pic.height = oh;
  } else {
    pic.rgba.assign(px, px + size_t(w) * h * 4);
    pic.width = w;
    pic.height = h;
  }
  stbi_image_free(px);
  return pic;
}
#endif

}  // namespace kk::workshop
