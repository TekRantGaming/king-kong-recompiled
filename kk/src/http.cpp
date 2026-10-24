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
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "platform.h"
#endif

namespace kk {

#if defined(_WIN32)
bool HttpGet(const std::string& url, std::vector<uint8_t>& out, std::atomic<uint64_t>* bytes,
             std::atomic<uint64_t>* total, int timeout_ms) {
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
  if (timeout_ms > 0) WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
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

bool HttpPost(const std::string& url, const std::string& json, std::vector<uint8_t>& out) {
  URL_COMPONENTSW parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256], path[2048];
  parts.lpszHostName = host;
  parts.dwHostNameLength = 256;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 2048;
  const std::wstring wurl(url.begin(), url.end());
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) return false;
  HINTERNET session = WinHttpOpen(L"KingKongRecomp/" KK_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return false;
  WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);
  bool ok = false;
  if (HINTERNET conn = WinHttpConnect(session, host, parts.nPort, 0)) {
    const DWORD secure = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    if (HINTERNET req = WinHttpOpenRequest(conn, L"POST", path, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, secure)) {
      DWORD code = 0, len = sizeof(code);
      if (WinHttpSendRequest(req, L"Content-Type: application/json\r\n", DWORD(-1),
                             const_cast<char*>(json.data()), DWORD(json.size()), DWORD(json.size()), 0) &&
          WinHttpReceiveResponse(req, nullptr) &&
          WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &len,
                              nullptr) &&
          code == 200) {
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
        }
      }
      WinHttpCloseHandle(req);
    }
    WinHttpCloseHandle(conn);
  }
  WinHttpCloseHandle(session);
  return ok;
}
int HttpPostBody(const std::string& url, const std::string& content_type, const std::vector<std::string>& headers,
                 const std::string& body, int timeout_ms, std::vector<uint8_t>& out, const std::string&) {
  URL_COMPONENTSW parts{};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256], path[2048];
  parts.lpszHostName = host;
  parts.dwHostNameLength = 256;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 2048;
  const std::wstring wurl(url.begin(), url.end());
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) return 0;
  HINTERNET session = WinHttpOpen(L"KingKongRecomp/" KK_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return 0;
  WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
  std::wstring head = L"Content-Type: " + std::wstring(content_type.begin(), content_type.end()) + L"\r\n";
  for (const auto& h : headers) head += std::wstring(h.begin(), h.end()) + L"\r\n";
  int status = 0;
  if (HINTERNET conn = WinHttpConnect(session, host, parts.nPort, 0)) {
    const DWORD secure = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    if (HINTERNET req = WinHttpOpenRequest(conn, L"POST", path, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, secure)) {
      DWORD code = 0, len = sizeof(code);
      if (WinHttpSendRequest(req, head.c_str(), DWORD(-1), const_cast<char*>(body.data()), DWORD(body.size()),
                             DWORD(body.size()), 0) &&
          WinHttpReceiveResponse(req, nullptr) &&
          WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &len,
                              nullptr)) {
        status = int(code);
        for (DWORD avail = 0; out.size() < 65536 && WinHttpQueryDataAvailable(req, &avail) && avail;) {
          const size_t at = out.size();
          out.resize(at + avail);
          DWORD read = 0;
          if (!WinHttpReadData(req, out.data() + at, avail, &read)) break;
          out.resize(at + read);
        }
      }
      WinHttpCloseHandle(req);
    }
    WinHttpCloseHandle(conn);
  }
  WinHttpCloseHandle(session);
  return status;
}
#else
// Linux: through curl (on SteamOS, Bazzite and the usual desktop distributions).
bool HttpGet(const std::string& url, std::vector<uint8_t>& out, std::atomic<uint64_t>* bytes,
             std::atomic<uint64_t>* total, int timeout_ms) {
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
  if (timeout_ms > 0)
    return RunCapture({"curl", "-sSfL", "-m", std::to_string((timeout_ms + 999) / 1000), "-A", agent, url}, out, bytes);
  return RunCapture({"curl", "-sSfL", "-A", agent, url}, out, bytes);
}

bool HttpPost(const std::string& url, const std::string& json, std::vector<uint8_t>& out) {
  const std::string agent = "KingKongRecomp/" KK_VERSION;
  return RunCapture({"curl", "-sSf", "-m", "10", "-A", agent, "-H", "Content-Type: application/json", "--data-binary",
                     json, url},
                    out);
}
int HttpPostBody(const std::string& url, const std::string& content_type, const std::vector<std::string>& headers,
                 const std::string& body, int timeout_ms, std::vector<uint8_t>& out, const std::string& scratch) {
  // curl reads the body from a file (it may be binary); the reply's status is all it prints.
  if (scratch.empty()) return 0;
  {
    std::ofstream f(scratch, std::ios::binary | std::ios::trunc);
    if (!f.write(body.data(), std::streamsize(body.size()))) return 0;
  }
  char seconds[32];
  std::snprintf(seconds, sizeof(seconds), "%.1f", std::max(0.5, timeout_ms / 1000.0));
  std::vector<std::string> argv = {"curl", "-sS", "-m", seconds, "-A", "KingKongRecomp/" KK_VERSION, "-o",
                                   "/dev/null", "-w", "%{http_code}", "-H", "Content-Type: " + content_type};
  for (const auto& h : headers) {
    argv.push_back("-H");
    argv.push_back(h);
  }
  argv.push_back("--data-binary");
  argv.push_back("@" + scratch);
  argv.push_back(url);
  std::vector<uint8_t> printed;
  const bool ran = RunCapture(argv, printed);
  std::error_code ec;
  std::filesystem::remove(scratch, ec);
  (void)out;
  if (!ran) return 0;
  return std::atoi(std::string(printed.begin(), printed.end()).c_str());
}
#endif

}  // namespace kk
