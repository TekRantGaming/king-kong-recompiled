// This file is UTF-8.
// Job J8: the Workshop page without a window or network: GitHub addresses, the README's first picture,
// the Markdown subset, the cache (ETag / Last-Modified, once a day) with a fake server, reply headers,
// JSON facts and picture decoding (two tiny pictures made for this test).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "workshop_logic.h"
#include "workshop_markdown.h"
#include "workshop_net.h"

#if !defined(_WIN32)
// workshop_net.cpp runs curl through platform.cpp on Linux; these tests never fetch.
namespace kk {
bool RunCapture(const std::vector<std::string>&, std::vector<uint8_t>&, std::atomic<uint64_t>*) { return false; }
}  // namespace kk
#endif

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

#include "workshop_test_pictures.inc"

using namespace kk::workshop;
namespace md = kk::workshop::md;

const Project& Pb() { return kProjects[0]; }

void Addresses() {
  Check(kProjectCount == 6, "six projects");
  Check(std::string(kProjects[4].name) == "Super Mario Sunshine" && std::string(kProjects[4].owner) == "chasem-dev" &&
            std::string(kProjects[4].repo) == "sms-launcher",
        "sms-launcher shown as Super Mario Sunshine");
  Check(std::string(kProjects[5].name) == "Dig Dug Native", "Dig Dug Native name");
  Check(RepoUrl(Pb()) == "https://github.com/TekRantGaming/point-blank-recompiled", "repository address");
  Check(ReleasesUrl(Pb()) == "https://github.com/TekRantGaming/point-blank-recompiled/releases/latest",
        "Download opens the latest release page");
  Check(ApiReadmeUrl(Pb()) == "https://api.github.com/repos/TekRantGaming/point-blank-recompiled/readme", "README API");
  Check(ApiRepoUrl(kProjects[4]) == "https://api.github.com/repos/chasem-dev/sms-launcher", "repository API");
  Check(RawReadmeUrl(Pb(), "") ==
            "https://raw.githubusercontent.com/TekRantGaming/point-blank-recompiled/HEAD/README.md",
        "raw README on the default branch (HEAD)");
  Check(RawReadmeUrl(Pb(), "main") ==
            "https://raw.githubusercontent.com/TekRantGaming/point-blank-recompiled/main/README.md",
        "raw README on a named branch");
  Check(SocialPreviewUrl(kProjects[5]) == "https://opengraph.githubassets.com/1/TekRantGaming/digdug-native",
        "GitHub's social preview card");
  for (const Project& p : kProjects) Check(ReleasesUrl(p).rfind("https://github.com/", 0) == 0, "https release page");

  const std::string raw = "https://raw.githubusercontent.com/TekRantGaming/point-blank-recompiled/main/";
  Check(ResolveUrl(Pb(), "main", "docs/images/banner.jpg", true) == raw + "docs/images/banner.jpg", "relative picture");
  Check(ResolveUrl(Pb(), "main", "./docs/a.png", true) == raw + "docs/a.png", "./ picture");
  Check(ResolveUrl(Pb(), "main", "/docs/a.png", true) == raw + "docs/a.png", "/ picture");
  Check(ResolveUrl(Pb(), "", "a.png", true) ==
            "https://raw.githubusercontent.com/TekRantGaming/point-blank-recompiled/HEAD/a.png",
        "picture without a known branch");
  Check(ResolveUrl(Pb(), "main", "docs/BUILDING.md", false) ==
            "https://github.com/TekRantGaming/point-blank-recompiled/blob/main/docs/BUILDING.md",
        "relative link to the file's page");
  Check(ResolveUrl(Pb(), "main", "#building", false).empty(), "anchor links are dropped");
  Check(ResolveUrl(Pb(), "main", "mailto:a@b.c", false).empty(), "mail links are dropped");
  Check(ResolveUrl(Pb(), "main", "javascript:alert(1)", false).empty(), "script links are dropped");
  Check(ResolveUrl(Pb(), "main", "//example.com/x", false) == "https://example.com/x", "//host made https");
  Check(ResolveUrl(Pb(), "main", "http://example.com/x", false) == "https://example.com/x", "http made https");
  Check(ResolveUrl(Pb(), "main", "https://github.com/o/r/blob/main/x.png", true) ==
            "https://github.com/o/r/raw/main/x.png",
        "a picture's blob page becomes its raw file");
  Check(ResolveUrl(Pb(), "main", " <https://a.b/c> ", false) == "https://a.b/c", "spaces and <> around an address");

  Check(IsAllowedImageHost("https://raw.githubusercontent.com/a/b/c.png"), "raw.githubusercontent allowed");
  Check(IsAllowedImageHost("https://github.com/user-attachments/assets/x"), "github.com allowed");
  Check(!IsAllowedImageHost("http://raw.githubusercontent.com/a.png"), "http not allowed");
  Check(!IsAllowedImageHost("https://evil.example/raw.githubusercontent.com/a.png"), "other hosts not allowed");
  Check(!IsAllowedImageHost("https://raw.githubusercontent.com.evil.example/a.png"), "look-alike host not allowed");
  Check(IsBadgeOrVector("https://img.shields.io/github/v/release/a/b?style=for-the-badge"), "shields badge");
  Check(IsBadgeOrVector("https://github.com/a/b/actions/workflows/build.yml/badge.svg"), "workflow badge");
  Check(IsBadgeOrVector("https://raw.githubusercontent.com/a/b/HEAD/logo.SVG?x=1"), "SVG picture");
  Check(IsBadgeOrVector("https://invidget.switchblade.xyz/abc"), "Discord invite widget");
  Check(!IsBadgeOrVector("https://raw.githubusercontent.com/a/b/HEAD/docs/banner.jpg"), "a photo is not a badge");
}

void ReadmePicture() {
  const std::string md =
      "<div align=\"center\">\n\n"
      "![build](https://github.com/TekRantGaming/digdug-native/actions/workflows/build.yml/badge.svg)\n"
      "[![Latest](https://img.shields.io/github/v/release/x/y)](https://github.com/x/y/releases/latest)\n"
      "<img src=\"https://invidget.switchblade.xyz/abc\" alt=\"Discord\">\n"
      "<img src=\"docs/icon.png\" height=\"40\" alt=\"small\">\n"
      "```\n![in code](docs/code.png)\n```\n"
      "<img src=\"docs/images/banner.jpg\" alt=\"Banner\" width=\"100%\">\n"
      "![second](docs/second.png)\n";
  Check(FirstReadmeImage(Pb(), "main", md) ==
            "https://raw.githubusercontent.com/TekRantGaming/point-blank-recompiled/main/docs/images/banner.jpg",
        "first README picture skips badges, widgets, icons and code");
  Check(FirstReadmeImage(Pb(), "main", "Text\n\n![Ready](docs/launcher-ready.png \"title\")\n") ==
            "https://raw.githubusercontent.com/TekRantGaming/point-blank-recompiled/main/docs/launcher-ready.png",
        "Markdown picture with a title");
  Check(FirstReadmeImage(Pb(), "main", "# Title\n\nNo pictures here.\n").empty(), "no picture: empty");
  Check(FirstReadmeImage(Pb(), "", "<IMG SRC='https://example.com/x.png'>").empty(), "pictures from other hosts skipped");
  Check(FirstReadmeImage(Pb(), "main", "<img width=\"260\" src=\"docs/images/gameplay.png\" alt=\"G\">") ==
            "https://raw.githubusercontent.com/TekRantGaming/point-blank-recompiled/main/docs/images/gameplay.png",
        "attribute order does not matter");

  Check(OgImage("<html><meta property=\"og:image\" content=\"https://opengraph.githubassets.com/abc/o/r\" />") ==
            "https://opengraph.githubassets.com/abc/o/r",
        "og:image (property first)");
  Check(OgImage("<meta content=\"https://repository-images.githubusercontent.com/1/2\" property=\"og:image\">") ==
            "https://repository-images.githubusercontent.com/1/2",
        "og:image (content first)");
  Check(OgImage("<meta property=\"og:title\" content=\"x\">").empty(), "no og:image");
  Check(HtmlAttr("<img src=a.png width=40>", "width") == "40" && HtmlAttr("<img src=a.png width=40>", "src") == "a.png",
        "unquoted attributes");
  Check(HtmlAttr("<img data-src=\"x\" src=\"y\">", "src") == "y", "data-src is not src");
}

void Json() {
  const std::string repo =
      "{\"id\": 1, \"full_name\": \"TekRantGaming/digdug-native\", \"owner\": {\"login\": \"TekRantGaming\"},"
      "\"description\": \"Dig Dug \\\"native\\\" \\u2014 PC port \\ud83c\\udfae\\nline\", \"stargazers_count\": 42,"
      "\"default_branch\": \"main\", \"pushed_at\": \"2026-10-13T22:17:50Z\","
      "\"license\": {\"key\": \"mit\", \"name\": \"MIT License\", \"spdx_id\": \"MIT\"}}";
  const RepoFacts f = ParseRepoFacts(repo);
  Check(f.ok, "repository facts parsed");
  Check(f.description == "Dig Dug \"native\" \xE2\x80\x94 PC port \xF0\x9F\x8E\xAE\nline", "JSON escapes and surrogates");
  Check(f.stars == 42 && f.default_branch == "main" && f.pushed == "2026-10-13" && f.license == "MIT", "facts");
  const RepoFacts n = ParseRepoFacts("{\"full_name\": \"a/b\", \"description\": null, \"license\": null}");
  Check(n.ok && n.description.empty() && n.license.empty() && n.stars == -1, "nulls and missing facts");
  Check(!ParseRepoFacts("{\"message\": \"API rate limit exceeded\"}").ok, "an API error is not facts");
  const RepoFacts o =
      ParseRepoFacts("{\"full_name\": \"a/b\", \"license\": {\"spdx_id\": \"NOASSERTION\", \"name\": \"Other\"}}");
  Check(o.license == "Other", "license without an SPDX id uses its name");
}

void CacheBookkeeping() {
  CacheMeta m{"https://a.b/c", "W/\"abc\"", "Tue, 13 Oct 2026 22:17:50 GMT", 1000};
  CacheMeta back;
  Check(ParseMeta(FormatMeta(m), back) && back.url == m.url && back.etag == m.etag &&
            back.last_modified == m.last_modified && back.fetched == 1000,
        "meta round trip");
  Check(!ParseMeta("", back), "empty meta");
  Check(CacheKey("https://a.b/c") == CacheKey("https://a.b/c") &&
            CacheKey("https://a.b/c") != CacheKey("https://a.b/d") && CacheKey("x").size() == 16,
        "cache keys");
  Check(IsFresh(m, 1000 + kRefreshSeconds - 1), "fresh within a day");
  Check(!IsFresh(m, 1000 + kRefreshSeconds), "stale after a day");
  Check(!IsFresh(m, 999), "a clock set back counts as stale");
  Check(!IsFresh(CacheMeta{}, 5), "never fetched is stale");
  const Headers h = ConditionalHeaders(m);
  Check(h.size() == 2 && h[0].first == "If-None-Match" && h[0].second == "W/\"abc\"" &&
            h[1].first == "If-Modified-Since",
        "conditional request headers");
  Check(ConditionalHeaders(CacheMeta{}).empty(), "no validators, no conditions");
  Check(DecideCacheStep(true, 200) == CacheStep::kStore && DecideCacheStep(false, 200) == CacheStep::kStore,
        "200 stores");
  Check(DecideCacheStep(true, 304) == CacheStep::kKeep, "304 keeps the copy");
  Check(DecideCacheStep(false, 304) == CacheStep::kFail, "304 without a copy fails");
  Check(DecideCacheStep(true, 0) == CacheStep::kUseStale && DecideCacheStep(true, 403) == CacheStep::kUseStale,
        "offline or rate limited: the saved copy");
  Check(DecideCacheStep(false, 0) == CacheStep::kFail, "offline without a copy fails");
  const ReplyHeaders r = ParseReplyHeaders(
      "HTTP/1.1 302 Found\r\nLocation: x\r\nETag: \"old\"\r\n\r\nHTTP/2 200\r\netag: \"new\"\r\n"
      "last-modified: Thu, 01 Oct 2026 12:25:23 GMT\r\n\r\n");
  Check(r.status == 200 && r.etag == "\"new\"" && r.last_modified == "Thu, 01 Oct 2026 12:25:23 GMT",
        "reply headers: the last reply after a redirect");
  Check(ParseReplyHeaders("HTTP/1.1 304 Not Modified\r\n").status == 304, "304 status line");
}

std::string Str(const std::vector<uint8_t>& b) { return std::string(b.begin(), b.end()); }

void CacheOnDisk(const std::filesystem::path& root) {
  std::error_code ec;
  const std::filesystem::path dir = root / "cache" / "workshop";
  std::filesystem::create_directories(root, ec);
  // A fresh folder for each run (the last one moved aside, never deleted).
  if (std::filesystem::exists(dir)) {
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    std::filesystem::rename(dir, root / ("old-" + std::to_string(stamp)), ec);
  }
  const std::string url = "https://api.github.com/repos/o/r/readme";
  int calls = 0;
  FetchRequest last;
  FetchReply next;
  Fetcher server = [&](const FetchRequest& rq) {
    ++calls;
    last = rq;
    return next;
  };
  const Headers accept = {{"Accept", "application/vnd.github.raw"}};
  next = {200, "\"v1\"", "Mon, 12 Oct 2026 10:00:00 GMT", {'#', ' ', 'A'}};
  CacheResult c = CachedGet(dir, url, accept, 1024, 1000, kRefreshSeconds, server);
  Check(c.ok && !c.from_cache && c.asked && c.status == 200 && Str(c.bytes) == "# A", "first fetch stores the body");
  Check(calls == 1 && last.headers.size() == 1, "first fetch: no conditions, the Accept header passed");
  Check(std::filesystem::exists(dir / (CacheKey(url) + ".bin")) &&
            std::filesystem::exists(dir / (CacheKey(url) + ".meta")),
        "cache files written in cache/workshop");
  c = CachedGet(dir, url, accept, 1024, 1000 + 3600, kRefreshSeconds, server);
  Check(c.ok && c.from_cache && !c.asked && calls == 1, "within a day: the cache, no request");
  next = {304, "", "", {}};
  c = CachedGet(dir, url, accept, 1024, 1000 + kRefreshSeconds + 5, kRefreshSeconds, server);
  bool none_match = false, since = false;
  for (const auto& [k, v] : last.headers) {
    none_match |= k == "If-None-Match" && v == "\"v1\"";
    since |= k == "If-Modified-Since" && v == "Mon, 12 Oct 2026 10:00:00 GMT";
  }
  Check(calls == 2 && none_match && since, "after a day: a conditional request with ETag and Last-Modified");
  Check(c.ok && c.from_cache && c.status == 304 && Str(c.bytes) == "# A", "304: the cached copy");
  c = CachedGet(dir, url, accept, 1024, 1000 + kRefreshSeconds + 10, kRefreshSeconds, server);
  Check(calls == 2 && c.ok && !c.asked, "304 renews the day");
  next = {};  // offline
  c = CachedGet(dir, url, accept, 1024, 1000 + 3 * kRefreshSeconds, kRefreshSeconds, server);
  Check(calls == 3 && c.ok && c.from_cache && c.asked && c.status == 0, "offline: the saved copy");
  c = CachedGet(dir, url, accept, 1024, 1000 + 3 * kRefreshSeconds + 1, kRefreshSeconds, server);
  Check(calls == 4, "after a failure it asks again next time");
  next = {200, "\"v2\"", "", {'#', ' ', 'B'}};
  c = CachedGet(dir, url, accept, 1024, 1000 + 4 * kRefreshSeconds, kRefreshSeconds, server);
  Check(c.ok && !c.from_cache && Str(c.bytes) == "# B", "200 replaces the copy");
  c = CachedGet(dir, url, accept, 1024, 1000 + 40 * kRefreshSeconds, kRefreshSeconds, nullptr);
  Check(c.ok && c.from_cache && !c.asked && Str(c.bytes) == "# B", "no network at all: the copy at any age");
  next = {};
  c = CachedGet(dir, "https://api.github.com/repos/o/other/readme", accept, 1024, 5, kRefreshSeconds, server);
  Check(!c.ok, "offline without a copy fails");
  c = CachedGet(dir, "https://api.github.com/repos/o/other/readme", accept, 1024, 5, kRefreshSeconds, nullptr);
  Check(!c.ok, "no network and no copy fails");
  next = {200, "", "", {'x'}};
  c = CachedGet(dir, url + "?2", accept, 1024, 5, kRefreshSeconds, server);
  Check(c.ok && !c.from_cache, "another address, its own entry");
  Check(HttpFetch({"http://example.com/", {}, 10}).status == 0, "http (not https) is never fetched");
}

std::string Plain(const md::Block& b) { return md::PlainText(b.spans); }

void Markdown() {
  using md::BlockKind;
  auto blocks = md::Parse(
      "# Title\n\nFirst line\nsecond **bold** and *it* and `code` and snake_case_name.\n\n"
      "Sub\n---\n\n- one\n- two\n  - nested\n1. first\n2. second\n\n"
      "```cpp\nint main() { return 0; }\n  indented\n```\n\n"
      "> quoted **text**\n\n***\n\n"
      "| Name | Value |\n|------|:-----:|\n| a \\| b | **2** |\n| c | [d](https://e.f) |\n\nAfter.");
  size_t i = 0;
  auto next = [&](BlockKind k, const char* what) -> const md::Block* {
    if (i >= blocks.size() || blocks[i].kind != k) {
      Check(false, what);
      return nullptr;
    }
    return &blocks[i++];
  };
  if (auto* b = next(BlockKind::kHeading, "h1")) Check(b->level == 1 && Plain(*b) == "Title", "h1 text");
  if (auto* b = next(BlockKind::kParagraph, "paragraph")) {
    Check(Plain(*b) == "First line second bold and it and code and snake_case_name.", "paragraph joined, markers gone");
    bool bold = false, it = false, code = false;
    for (const auto& s : b->spans) {
      bold |= s.bold && s.text == "bold";
      it |= s.italic && s.text == "it";
      code |= s.code && s.text == "code";
    }
    Check(bold && it && code, "bold, italic and code spans");
  }
  if (auto* b = next(BlockKind::kHeading, "setext h2")) Check(b->level == 2 && Plain(*b) == "Sub", "setext h2 text");
  if (auto* b = next(BlockKind::kListItem, "item one"))
    Check(b->number == 0 && b->level == 0 && Plain(*b) == "one", "bullet");
  next(BlockKind::kListItem, "item two");
  if (auto* b = next(BlockKind::kListItem, "nested")) Check(b->level == 1, "nested level");
  if (auto* b = next(BlockKind::kListItem, "ordered")) Check(b->number == 1 && Plain(*b) == "first", "ordered 1");
  if (auto* b = next(BlockKind::kListItem, "ordered 2")) Check(b->number == 2, "ordered 2");
  if (auto* b = next(BlockKind::kCode, "code block"))
    Check(b->code == "int main() { return 0; }\n  indented", "code block kept as written");
  if (auto* b = next(BlockKind::kQuote, "quote")) Check(Plain(*b) == "quoted text", "quote text");
  next(BlockKind::kRule, "rule");
  if (auto* b = next(BlockKind::kTableRow, "table header"))
    Check(b->header && b->cells.size() == 2 && md::PlainText(b->cells[0]) == "Name", "table header row");
  if (auto* b = next(BlockKind::kTableRow, "table row"))
    Check(!b->header && md::PlainText(b->cells[0]) == "a | b" && !b->cells[1].empty() && b->cells[1][0].bold,
          "escaped pipe and bold cell");
  if (auto* b = next(BlockKind::kTableRow, "table row 2"))
    Check(b->cells.size() == 2 && b->cells[1].size() == 1 && b->cells[1][0].link == "https://e.f", "link in a cell");
  if (auto* b = next(BlockKind::kParagraph, "after")) Check(Plain(*b) == "After.", "paragraph after the table");
  Check(i == blocks.size(), "no extra blocks");

  // Links: resolved, anchors dropped, autolinks and bare addresses.
  const md::Resolver resolve = [](const std::string& u, bool picture) {
    return ResolveUrl(kProjects[0], "main", u, picture);
  };
  blocks = md::Parse("See [the docs](docs/BUILD.md), [top](#top), <https://a.b/c> and https://x.y/z.", resolve);
  Check(blocks.size() == 1, "one paragraph of links");
  if (!blocks.empty()) {
    std::vector<std::string> links;
    for (const auto& s : blocks[0].spans)
      if (!s.link.empty()) links.push_back(s.link);
    Check(links.size() == 3 &&
              links[0] == "https://github.com/TekRantGaming/point-blank-recompiled/blob/main/docs/BUILD.md" &&
              links[1] == "https://a.b/c" && links[2] == "https://x.y/z",
          "link targets (relative, autolink, bare; anchor dropped)");
    Check(Plain(blocks[0]) == "See the docs, top, https://a.b/c and https://x.y/z.", "link texts");
  }

  // README HTML: centred banner, badges in links, pictures side by side in a table, cells as paragraphs.
  blocks = md::Parse(
      "<div align=\"center\">\n\n<img src=\"docs/images/banner.jpg\" alt=\"Banner\" width=\"100%\">\n\n<br>\n\n"
      "[![Latest release](https://img.shields.io/x)](https://github.com/o/r/releases/latest)\n"
      "![Platforms](https://img.shields.io/y)\n\n"
      "### Point Blank &amp; more<br>on PC\n\n"
      "<sub>Made with <a href=\"https://github.com/mstan/psxrecomp\">PSXRecomp</a>. <b>No game files</b>.</sub>\n\n"
      "</div>\n\n<!-- a comment\nover lines -->\n\n"
      "<table>\n<tr>\n<td width=\"50%\"><img src=\"docs/a.jpg\" alt=\"A\"></td>\n"
      "<td width=\"50%\"><img src=\"docs/b.jpg\" alt=\"B\"></td>\n</tr>\n<tr>\n"
      "<td><img src=\"docs/c.jpg\" alt=\"C\"></td>\n<td valign=\"top\">\n\n**Every mode:** Arcade.\n\n</td>\n</tr>\n"
      "</table>\n\n"
      "<h2>Install</h2>\n<ul><li>Step one</li><li>Step two</li></ul>\n<p>Done &copy; 2026 &#8212; ok</p>",
      resolve);
  i = 0;
  if (auto* b = next(BlockKind::kImages, "banner")) {
    Check(b->images.size() == 1 && b->images[0].width_pct == 100 &&
              b->images[0].src ==
                  "https://raw.githubusercontent.com/TekRantGaming/point-blank-recompiled/main/docs/images/banner.jpg",
          "banner picture resolved, width 100%");
  }
  if (auto* b = next(BlockKind::kImages, "badges")) {
    Check(b->images.size() == 2 && b->images[0].link == "https://github.com/o/r/releases/latest" &&
              b->images[0].alt == "Latest release" && b->images[1].link.empty(),
          "badges: one in a link, one not");
  }
  if (auto* b = next(BlockKind::kHeading, "h3 with entity and break"))
    Check(b->level == 3 && Plain(*b) == "Point Blank & more\non PC", "heading entity and <br>");
  if (auto* b = next(BlockKind::kParagraph, "sub paragraph")) {
    Check(Plain(*b) == "Made with PSXRecomp. No game files.", "HTML tags stripped, text kept");
    bool link = false, bold = false;
    for (const auto& s : b->spans) {
      link |= s.text == "PSXRecomp" && s.link == "https://github.com/mstan/psxrecomp";
      bold |= s.text == "No game files" && s.bold;
    }
    Check(link && bold, "<a href> and <b>");
  }
  if (auto* b = next(BlockKind::kImages, "table row of pictures")) Check(b->images.size() == 2, "two pictures side by side");
  if (auto* b = next(BlockKind::kImages, "third picture"))
    Check(b->images.size() == 1 && b->images[0].alt == "C", "next row");
  if (auto* b = next(BlockKind::kParagraph, "cell text")) Check(Plain(*b) == "Every mode: Arcade.", "cell text paragraph");
  if (auto* b = next(BlockKind::kHeading, "h2 tag")) Check(b->level == 2 && Plain(*b) == "Install", "<h2>");
  if (auto* b = next(BlockKind::kListItem, "li 1")) Check(Plain(*b) == "Step one", "<li> one");
  if (auto* b = next(BlockKind::kListItem, "li 2")) Check(Plain(*b) == "Step two", "<li> two");
  if (auto* b = next(BlockKind::kParagraph, "p")) Check(Plain(*b) == "Done \xC2\xA9 2026 \xE2\x80\x94 ok", "entities");
  Check(i == blocks.size(), "no extra HTML blocks");

  // Odds and ends: escapes, lone markers, alerts, task lists.
  blocks = md::Parse(
      "2 * 3 = 6, a < b, \\*not italic\\*, 5_000_000 and __init__.\n\n> [!NOTE]\n> Read this.\n\n- [x] done");
  Check(blocks.size() == 3, "three blocks");
  if (blocks.size() == 3) {
    Check(Plain(blocks[0]) == "2 * 3 = 6, a < b, *not italic*, 5_000_000 and init.", "lone markers and escapes");
    Check(blocks[1].kind == BlockKind::kQuote && Plain(blocks[1]).rfind("Note", 0) == 0, "GitHub alert as a bold label");
    Check(blocks[2].kind == BlockKind::kListItem && Plain(blocks[2]) == "\xE2\x98\x91 done", "task list box");
  }
  Check(md::DecodeEntities("&lt;b&gt; &amp;amp; &#x41;&#66; &bogus; &") == "<b> &amp; AB &bogus; &", "entity decoding");
  Check(md::Parse("").empty() && md::Parse("\n\n   \n").empty(), "empty README");
  // A long, odd README parses fully (no runaway on lone brackets and markers).
  std::string big;
  for (int k = 0; k < 4000; ++k) big += "Line [with *odd* `marks` and <tags and [brackets\n";
  Check(!md::Parse(big).empty(), "a long, odd README parses");
}

void Pictures() {
  const std::vector<uint8_t> png(kTestPng, kTestPng + sizeof(kTestPng));
  const std::vector<uint8_t> jpeg(kTestJpeg, kTestJpeg + sizeof(kTestJpeg));
  Check(LooksLikePicture(png) && LooksLikePicture(jpeg), "PNG and JPEG recognised");
  const std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>";
  Check(!LooksLikePicture(std::vector<uint8_t>(svg.begin(), svg.end())), "SVG is not decoded");
  Picture p = DecodePicture(png, 1280);
  Check(p.width == 32 && p.height == 16 && p.rgba.size() == 32 * 16 * 4, "PNG decoded");
  if (p) Check(p.rgba[0] > 200 && p.rgba[2] < 60 && p.rgba[3] == 255 && p.rgba[31 * 4 + 2] > 200, "PNG colours");
  p = DecodePicture(jpeg, 1280);
  Check(p.width == 32 && p.height == 16, "JPEG decoded");
  if (p) Check(p.rgba[0] > 170 && p.rgba[31 * 4 + 2] > 170, "JPEG colours roughly kept");
  p = DecodePicture(png, 8);
  Check(p.width == 8 && p.height == 4, "shrunk to the width limit, aspect kept");
  std::vector<uint8_t> broken = png;
  broken.resize(40);
  Check(!DecodePicture(broken, 100), "a cut-off PNG fails cleanly");
  Check(!DecodePicture({}, 100), "nothing fails cleanly");
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path root =
      argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::temp_directory_path() / "kk-workshop-test";
  Addresses();
  ReadmePicture();
  Json();
  CacheBookkeeping();
  CacheOnDisk(root);
  Markdown();
  Pictures();
  if (failures) {
    std::printf("%d workshop check(s) failed\n", failures);
    return 1;
  }
  std::printf("workshop: addresses, README pictures, Markdown, cache and picture decoding passed\n");
  return 0;
}
