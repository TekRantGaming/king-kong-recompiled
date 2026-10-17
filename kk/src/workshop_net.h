// Job J8: the Workshop's network and disk: HTTPS GET with request headers and the reply's validators
// (WinHTTP on Windows, curl elsewhere, as content.cpp and http.cpp do), the cache in the user folder
// (<user data>/cache/workshop: <key>.bin + <key>.meta, refreshed at most once a day with ETag /
// Last-Modified), and picture decoding (PNG, JPEG, GIF: Windows Imaging Component on Windows, stb_image
// elsewhere). All of it runs on the Workshop's worker thread, never on the launcher's UI thread.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "workshop_logic.h"

namespace kk::workshop {

struct FetchRequest {
  std::string url;
  Headers headers;
  size_t max_bytes = kMaxTextBytes;
};

struct FetchReply {
  int status = 0;  // 0: no reply (offline, timeout, too big, not https)
  std::string etag, last_modified;
  std::vector<uint8_t> body;
};

using Fetcher = std::function<FetchReply(const FetchRequest&)>;

// The system's HTTPS GET (https:// addresses only). `stop` ends a transfer early.
FetchReply HttpFetch(const FetchRequest& request, const std::atomic<bool>* stop = nullptr);

struct CacheResult {
  bool ok = false;          // bytes holds the body (new or cached)
  bool from_cache = false;  // the cached copy (fresh, confirmed by a 304, or stale after an error)
  bool asked = false;       // the server was asked
  int status = 0;           // the server's answer when asked
  std::vector<uint8_t> bytes;
};

// The body of `url`: the cached copy while it is less than `max_age` seconds old, else asked again with
// its validators (304 keeps the copy, 200 replaces it, an error falls back to the copy). A null `fetch`
// means offline: the copy at any age.
CacheResult CachedGet(const std::filesystem::path& dir, const std::string& url, const Headers& headers,
                      size_t max_bytes, int64_t now, int64_t max_age, const Fetcher& fetch);

struct Picture {
  int width = 0, height = 0;
  std::vector<uint8_t> rgba;
  explicit operator bool() const { return width > 0 && height > 0; }
};

// Whether bytes start like a PNG, JPEG or GIF (nothing else is decoded).
bool LooksLikePicture(const std::vector<uint8_t>& bytes);

// Decodes a PNG, JPEG or GIF (first frame), shrunk to at most `max_width` pixels wide. Empty on failure
// or when it is larger than 8192 pixels either way.
Picture DecodePicture(const std::vector<uint8_t>& bytes, int max_width);

}  // namespace kk::workshop
