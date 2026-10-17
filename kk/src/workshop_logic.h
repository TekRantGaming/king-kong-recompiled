// Job J8 (owner, 17 Oct): the launcher's Workshop page, the owner's other projects as tiles with their
// GitHub README. This header is the part without a window, network or disk, so it can be unit tested
// (tests/workshop_tests.cpp): the project list, the GitHub addresses, the picture a README starts with,
// the cache's bookkeeping (ETag / Last-Modified, refreshed at most once a day) and the reply headers.
//
// Nothing of the projects is shipped or committed: names and addresses only. Pictures and READMEs are
// fetched at run time and cached in the user folder (workshop.cpp).

#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kk::workshop {

struct Project {
  const char* name;  // shown as is (owner, 17 Oct: these exact names; the South Park ones from each README's title)
  const char* owner;
  const char* repo;
};

inline constexpr Project kProjects[] = {
    {"Point Blank Recompiled", "TekRantGaming", "point-blank-recompiled"},
    {"Simpsons Wrestling Recompiled", "TekRantGaming", "simpsons-wrestling-recompiled"},
    {"Earthworm Jim HD Recompiled", "TekRantGaming", "earthworm-jim-hd-recompiled"},
    {"Outpost Kaloki X Recompiled", "TekRantGaming", "outpost-kaloki-x-recompiled"},
    {"Super Mario Sunshine", "chasem-dev", "sms-launcher"},
    {"Dig Dug Native", "TekRantGaming", "digdug-native"},
    {"South Park: Tenorman's Revenge Recompiled", "TekRantGaming", "south-park-tenormans-revenge-recompiled"},
    {"South Park: Let's Go Tower Defense Play! Recompiled", "TekRantGaming", "south-park-tower-defense-recompiled"},
};
inline constexpr int kProjectCount = int(sizeof(kProjects) / sizeof(kProjects[0]));

// A day: README, repository facts and pictures are asked for again at most this often (GitHub allows 60
// unauthenticated API requests an hour per address; a 304 reply to a conditional request does not count).
inline constexpr int64_t kRefreshSeconds = 24 * 60 * 60;
// Size limits for what is fetched (a README, a page, a picture).
inline constexpr size_t kMaxTextBytes = 2u << 20;
inline constexpr size_t kMaxImageBytes = 12u << 20;
// README pictures fetched per project (the rest are left out).
inline constexpr int kMaxReadmeImages = 16;

// ------------------------------------------------------------- addresses ---
inline std::string RepoUrl(const Project& p) { return std::string("https://github.com/") + p.owner + "/" + p.repo; }
// [Download]: the latest release's page, opened in the browser (nothing is downloaded or run by the launcher).
inline std::string ReleasesUrl(const Project& p) { return RepoUrl(p) + "/releases/latest"; }
inline std::string ApiRepoUrl(const Project& p) {
  return std::string("https://api.github.com/repos/") + p.owner + "/" + p.repo;
}
// With "Accept: application/vnd.github.raw" the reply is the README's Markdown itself.
inline std::string ApiReadmeUrl(const Project& p) { return ApiRepoUrl(p) + "/readme"; }
inline std::string RawBase(const Project& p, const std::string& branch) {
  return std::string("https://raw.githubusercontent.com/") + p.owner + "/" + p.repo + "/" +
         (branch.empty() ? "HEAD" : branch) + "/";
}
// When the API does not answer (rate limit): the default branch's README.md, not counted by the API.
inline std::string RawReadmeUrl(const Project& p, const std::string& branch) { return RawBase(p, branch) + "README.md"; }
// GitHub's generated social preview, when the README has no picture and the page names no og:image.
inline std::string SocialPreviewUrl(const Project& p) {
  return std::string("https://opengraph.githubassets.com/1/") + p.owner + "/" + p.repo;
}

inline bool StartsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

inline std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// The host of an http(s) address, lower case ("" if it is not one).
inline std::string UrlHost(std::string_view url) {
  size_t at = 0;
  if (StartsWith(url, "https://")) at = 8;
  else if (StartsWith(url, "http://")) at = 7;
  else return {};
  const size_t end = url.find_first_of("/?#:", at);
  return Lower(std::string(url.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at)));
}

// Pictures are only fetched from GitHub's own hosts (a README could point anywhere).
inline bool IsAllowedImageHost(std::string_view url) {
  if (!StartsWith(url, "https://")) return false;
  const std::string host = UrlHost(url);
  static constexpr const char* kHosts[] = {
      "raw.githubusercontent.com",          "github.com",
      "user-images.githubusercontent.com",  "private-user-images.githubusercontent.com",
      "camo.githubusercontent.com",         "opengraph.githubassets.com",
      "repository-images.githubusercontent.com", "avatars.githubusercontent.com",
      "objects.githubusercontent.com"};
  for (const char* h : kHosts)
    if (host == h) return true;
  return false;
}

// Badges (shields.io, workflow badges, invite widgets) and SVG pictures: not photos, and not decodable.
inline bool IsBadgeOrVector(std::string_view url) {
  const std::string u = Lower(std::string(url.substr(0, url.find_first_of("?#"))));
  const std::string host = UrlHost(u);
  if (host == "img.shields.io" || host == "badgen.net" || host == "badge.fury.io" ||
      host == "invidget.switchblade.xyz" || host == "discordapp.com" || host == "discord.com")
    return true;
  if (u.size() >= 4 && u.compare(u.size() - 4, 4, ".svg") == 0) return true;
  return u.find("/badge.svg") != std::string::npos || u.find("/badges/") != std::string::npos;
}

// A README's address (a picture or a link) made absolute: relative paths are the repository's own files
// (pictures from raw.githubusercontent.com, links to the file's page on github.com), "//host" becomes
// https, a picture's "github.com/.../blob/..." its raw file. Anchors, mail and other schemes give "".
inline std::string ResolveUrl(const Project& p, const std::string& branch, std::string url, bool image) {
  while (!url.empty() && std::isspace(static_cast<unsigned char>(url.front()))) url.erase(url.begin());
  while (!url.empty() && std::isspace(static_cast<unsigned char>(url.back()))) url.pop_back();
  if (url.size() >= 2 && url.front() == '<' && url.back() == '>') url = url.substr(1, url.size() - 2);
  if (url.empty() || url[0] == '#') return {};
  if (StartsWith(url, "//")) url = "https:" + url;
  if (StartsWith(url, "http://")) url = "https://" + url.substr(7);
  if (StartsWith(url, "https://")) {
    if (image && StartsWith(url, "https://github.com/") && url.find("/blob/") != std::string::npos) {
      const size_t at = url.find("/blob/");
      url = url.substr(0, at) + "/raw/" + url.substr(at + 6);
    }
    return url;
  }
  // Another scheme (mailto:, data:, javascript:...).
  const size_t colon = url.find(':'), slash = url.find('/');
  if (colon != std::string::npos && (slash == std::string::npos || colon < slash)) return {};
  // A path in the repository (the README is at its root).
  while (StartsWith(url, "./")) url = url.substr(2);
  while (StartsWith(url, "../")) url = url.substr(3);
  while (!url.empty() && url[0] == '/') url.erase(url.begin());
  if (url.empty()) return {};
  if (image) return RawBase(p, branch) + url;
  return RepoUrl(p) + "/blob/" + (branch.empty() ? std::string("HEAD") : branch) + "/" + url;
}

// --------------------------------------------------------------- HTML bits ---
// The value of an attribute in a tag's text (`<img src="a" width=40>`), "" if missing.
inline std::string HtmlAttr(std::string_view tag, std::string_view name) {
  const std::string lower = Lower(std::string(tag));
  for (size_t at = 0; (at = lower.find(name, at)) != std::string::npos; ++at) {
    if (at > 0 && !std::isspace(static_cast<unsigned char>(lower[at - 1]))) continue;
    size_t i = at + name.size();
    while (i < lower.size() && std::isspace(static_cast<unsigned char>(lower[i]))) ++i;
    if (i >= lower.size() || lower[i] != '=') continue;
    ++i;
    while (i < lower.size() && std::isspace(static_cast<unsigned char>(lower[i]))) ++i;
    if (i >= lower.size()) return {};
    if (lower[i] == '"' || lower[i] == '\'') {
      const char q = lower[i];
      const size_t end = lower.find(q, i + 1);
      return std::string(tag.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1));
    }
    size_t end = i;
    while (end < lower.size() && !std::isspace(static_cast<unsigned char>(lower[end])) && lower[end] != '>' &&
           lower[end] != '/')
      ++end;
    return std::string(tag.substr(i, end - i));
  }
  return {};
}

// A size attribute in pixels (width="260"), 0 when missing or a percentage.
inline int HtmlPixels(const std::string& value) {
  if (value.empty() || value.find('%') != std::string::npos) return 0;
  return std::atoi(value.c_str());
}

// The og:image of a repository's page (its social preview: the owner's own picture or GitHub's card).
inline std::string OgImage(const std::string& html) {
  for (size_t at = 0; (at = html.find("<meta", at)) != std::string::npos; at += 5) {
    const size_t end = html.find('>', at);
    if (end == std::string::npos) break;
    const std::string_view tag(html.data() + at, end - at);
    if (HtmlAttr(tag, "property") == "og:image" || HtmlAttr(tag, "name") == "og:image") return HtmlAttr(tag, "content");
  }
  return {};
}

// The tile's picture: the README's first picture that is a photo from GitHub (not a badge, an SVG, a
// tiny icon or a picture inside a code block), made absolute. "" when it has none.
inline std::string FirstReadmeImage(const Project& p, const std::string& branch, const std::string& md) {
  bool fence = false;
  size_t line_start = 0;
  auto usable = [&](const std::string& url) {
    return !url.empty() && !IsBadgeOrVector(url) && IsAllowedImageHost(url);
  };
  while (line_start <= md.size()) {
    size_t line_end = md.find('\n', line_start);
    if (line_end == std::string::npos) line_end = md.size();
    const std::string line = md.substr(line_start, line_end - line_start);
    size_t first = line.find_first_not_of(" \t");
    if (first != std::string::npos && (line.compare(first, 3, "```") == 0 || line.compare(first, 3, "~~~") == 0)) {
      fence = !fence;
    } else if (!fence) {
      for (size_t i = 0; i < line.size(); ++i) {
        if (line.compare(i, 2, "![") == 0) {
          const size_t close = line.find("](", i + 2);
          if (close == std::string::npos) continue;
          const size_t end = line.find(')', close + 2);
          if (end == std::string::npos) continue;
          std::string target = line.substr(close + 2, end - close - 2);
          if (const size_t sp = target.find(' '); sp != std::string::npos) target = target.substr(0, sp);  // a title
          const std::string url = ResolveUrl(p, branch, target, true);
          if (usable(url)) return url;
          i = end;
        } else if (line[i] == '<' && Lower(line.substr(i, 4)) == "<img") {
          const size_t end = line.find('>', i);
          const std::string_view tag(line.data() + i, (end == std::string::npos ? line.size() : end) - i);
          const int w = HtmlPixels(HtmlAttr(tag, "width")), h = HtmlPixels(HtmlAttr(tag, "height"));
          const std::string url = ResolveUrl(p, branch, HtmlAttr(tag, "src"), true);
          if ((w == 0 || w > 64) && (h == 0 || h > 64) && usable(url)) return url;
          if (end == std::string::npos) break;
          i = end;
        }
      }
    }
    line_start = line_end + 1;
  }
  return {};
}

// ------------------------------------------------------------------- JSON ---
// The string value of the first "key": "..." in a JSON text, with its escapes (\n, \", \uXXXX...)
// decoded to UTF-8. `found` says whether it was there as a string (null and numbers are not).
inline void AppendUtf8(std::string& out, uint32_t c) {
  if (c < 0x80) {
    out += char(c);
  } else if (c < 0x800) {
    out += char(0xC0 | (c >> 6));
    out += char(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    out += char(0xE0 | (c >> 12));
    out += char(0x80 | ((c >> 6) & 0x3F));
    out += char(0x80 | (c & 0x3F));
  } else {
    out += char(0xF0 | (c >> 18));
    out += char(0x80 | ((c >> 12) & 0x3F));
    out += char(0x80 | ((c >> 6) & 0x3F));
    out += char(0x80 | (c & 0x3F));
  }
}

inline std::string JsonString(const std::string& json, const std::string& key, bool* found = nullptr) {
  if (found) *found = false;
  const std::string needle = "\"" + key + "\"";
  for (size_t k = json.find(needle); k != std::string::npos; k = json.find(needle, k + 1)) {
    size_t c = json.find_first_not_of(" \t\r\n", k + needle.size());
    if (c == std::string::npos || json[c] != ':') continue;
    size_t q = json.find_first_not_of(" \t\r\n", c + 1);
    if (q == std::string::npos || json[q] != '"') return {};
    std::string out;
    for (size_t i = q + 1; i < json.size(); ++i) {
      const char ch = json[i];
      if (ch == '"') {
        if (found) *found = true;
        return out;
      }
      if (ch != '\\' || i + 1 >= json.size()) {
        out += ch;
        continue;
      }
      const char e = json[++i];
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': break;
        case 'b': case 'f': break;
        case 'u': {
          if (i + 4 >= json.size()) return out;
          uint32_t cp = uint32_t(std::strtoul(json.substr(i + 1, 4).c_str(), nullptr, 16));
          i += 4;
          if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < json.size() && json[i + 1] == '\\' && json[i + 2] == 'u') {
            const uint32_t lo = uint32_t(std::strtoul(json.substr(i + 3, 4).c_str(), nullptr, 16));
            if (lo >= 0xDC00 && lo < 0xE000) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              i += 6;
            }
          }
          AppendUtf8(out, cp);
          break;
        }
        default: out += e; break;  // \" \\ \/
      }
    }
    return out;
  }
  return {};
}

// A number value ("stargazers_count": 12), -1 when missing.
inline long long JsonNumber(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  const size_t k = json.find(needle);
  if (k == std::string::npos) return -1;
  size_t c = json.find_first_not_of(" \t\r\n", k + needle.size());
  if (c == std::string::npos || json[c] != ':') return -1;
  c = json.find_first_not_of(" \t\r\n", c + 1);
  if (c == std::string::npos || !(std::isdigit(static_cast<unsigned char>(json[c])) || json[c] == '-')) return -1;
  return std::strtoll(json.c_str() + c, nullptr, 10);
}

// What the tiles and the detail view show besides the README (from api.github.com/repos/<o>/<r>).
struct RepoFacts {
  std::string description, default_branch, pushed, license;
  long long stars = -1;
  bool ok = false;
};

inline RepoFacts ParseRepoFacts(const std::string& json) {
  RepoFacts f;
  bool has_name = false;
  JsonString(json, "full_name", &has_name);
  if (!has_name) return f;
  f.ok = true;
  f.description = JsonString(json, "description");
  f.default_branch = JsonString(json, "default_branch");
  f.pushed = JsonString(json, "pushed_at").substr(0, 10);  // the date of 2026-10-13T22:17:50Z
  f.stars = JsonNumber(json, "stargazers_count");
  // "license": {"key": "mit", "name": "MIT License", "spdx_id": "MIT", ...}
  if (const size_t at = json.find("\"license\""); at != std::string::npos) {
    const std::string tail = json.substr(at);
    if (tail.find_first_not_of(" \t\r\n:", 9) != std::string::npos && tail[tail.find_first_not_of(" \t\r\n:", 9)] == '{') {
      f.license = JsonString(tail, "spdx_id");
      if (f.license.empty() || f.license == "NOASSERTION") f.license = JsonString(tail, "name");
    }
  }
  return f;
}

// ------------------------------------------------------------------ cache ---
// Each cached address is <key>.bin (the reply's body) and <key>.meta (these lines) in the cache folder.
struct CacheMeta {
  std::string url, etag, last_modified;
  int64_t fetched = 0;  // seconds since 1970 of the last 200 or 304 from the server
};

// FNV-1a of the address, as 16 hex digits.
inline std::string CacheKey(const std::string& url) {
  uint64_t h = 1469598103934665603ull;
  for (const unsigned char c : url) {
    h ^= c;
    h *= 1099511628211ull;
  }
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

inline std::string FormatMeta(const CacheMeta& m) {
  return "url=" + m.url + "\netag=" + m.etag + "\nlast_modified=" + m.last_modified +
         "\nfetched=" + std::to_string(m.fetched) + "\n";
}

inline bool ParseMeta(const std::string& text, CacheMeta& m) {
  m = {};
  size_t start = 0;
  bool any = false;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t eq = line.find('=');
    if (eq != std::string::npos) {
      const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
      if (k == "url") m.url = v, any = true;
      else if (k == "etag") m.etag = v;
      else if (k == "last_modified") m.last_modified = v;
      else if (k == "fetched") m.fetched = std::strtoll(v.c_str(), nullptr, 10);
    }
    start = end + 1;
  }
  return any;
}

// Fresh: fetched less than `max_age` seconds ago (a clock set back counts as stale).
inline bool IsFresh(const CacheMeta& m, int64_t now, int64_t max_age = kRefreshSeconds) {
  return m.fetched > 0 && now >= m.fetched && now - m.fetched < max_age;
}

using Headers = std::vector<std::pair<std::string, std::string>>;

// If-None-Match / If-Modified-Since for a cached copy (a 304 reply keeps it).
inline Headers ConditionalHeaders(const CacheMeta& m) {
  Headers h;
  if (!m.etag.empty()) h.push_back({"If-None-Match", m.etag});
  if (!m.last_modified.empty()) h.push_back({"If-Modified-Since", m.last_modified});
  return h;
}

// What to do with a server's reply (status 0: no reply at all).
enum class CacheStep {
  kStore,      // 200: keep the new body and its validators
  kKeep,       // 304 with a cached copy: the copy is current (its time is renewed)
  kUseStale,   // an error with a cached copy: show the copy, ask again next time
  kFail,       // an error and nothing cached
};

inline CacheStep DecideCacheStep(bool have_cache, int status) {
  if (status == 200) return CacheStep::kStore;
  if (status == 304 && have_cache) return CacheStep::kKeep;
  return have_cache ? CacheStep::kUseStale : CacheStep::kFail;
}

// The status, ETag and Last-Modified of a reply's headers as curl -D prints them (redirects first: the
// last reply counts).
struct ReplyHeaders {
  int status = 0;
  std::string etag, last_modified;
};

inline ReplyHeaders ParseReplyHeaders(const std::string& text) {
  ReplyHeaders r;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::string lower = Lower(line);
    auto value = [&](size_t n) {
      std::string v = line.substr(n);
      while (!v.empty() && v.front() == ' ') v.erase(v.begin());
      return v;
    };
    if (StartsWith(lower, "http/")) {
      const size_t sp = line.find(' ');
      r = {};
      r.status = sp == std::string::npos ? 0 : std::atoi(line.c_str() + sp + 1);
    } else if (StartsWith(lower, "etag:")) {
      r.etag = value(5);
    } else if (StartsWith(lower, "last-modified:")) {
      r.last_modified = value(14);
    }
    start = end + 1;
  }
  return r;
}

}  // namespace kk::workshop
