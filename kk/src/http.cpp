#include "http.h"

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
#else
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

#include "platform.h"
#endif

namespace kk {

#if defined(_WIN32)
bool HttpGet(const std::string& url, std::vector<uint8_t>& out, std::atomic<uint64_t>* bytes,
             std::atomic<uint64_t>* total) {
  URL_COMPONENTSW parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256], path[2048];
  parts.lpszHostName = host;
  parts.dwHostNameLength = 256;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 2048;
  const std::wstring wurl(url.begin(), url.end());
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) return false;
  // GitHub's API needs a User-Agent; this is it.
  HINTERNET session = WinHttpOpen(L"KingKongRecomp/" KK_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
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
        if (total && WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, nullptr,
                                         &content, &len, nullptr))
          *total += content;
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
          if (bytes) *bytes += read;
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
// Linux: through curl (on SteamOS, Bazzite and the usual desktop distributions).
bool HttpGet(const std::string& url, std::vector<uint8_t>& out, std::atomic<uint64_t>* bytes,
             std::atomic<uint64_t>* total) {
  const std::string agent = "KingKongRecomp/" KK_VERSION;
  if (total) {
    // The size, from the last response's headers (GitHub downloads redirect).
    std::vector<uint8_t> head;
    if (RunCapture({"curl", "-sSfIL", "-A", agent, url}, head)) {
      uint64_t size = 0;
      std::stringstream lines(std::string(head.begin(), head.end()));
      for (std::string line; std::getline(lines, line);) {
        std::string lower = line;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        if (lower.rfind("content-length:", 0) == 0) size = std::strtoull(line.c_str() + 15, nullptr, 10);
      }
      *total += size;
    }
  }
  return RunCapture({"curl", "-sSfL", "-A", agent, url}, out, bytes);
}
#endif

}  // namespace kk
