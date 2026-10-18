// This file is UTF-8.
#include "workshop.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <random>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#include <rex/logging.h>
#include <rex/ui/immediate_drawer.h>

#include "launcher_text.h"
#include "mods.h"
#include "platform.h"
#include "workshop_logic.h"
#include "workshop_markdown.h"
#include "workshop_mods.h"
#include "workshop_texpacks.h"
#include "workshop_net.h"

namespace kk {

namespace fs = std::filesystem;
namespace ws = kk::workshop;
namespace md = kk::workshop::md;
namespace color = ui::color;
using ui::Action;
using text::F;
using text::T;

namespace {

#define KK_WS_DOT "\xC2\xB7"

int64_t Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Text the launcher's fonts can draw: emoji (4-byte UTF-8), the emoji variation selector and the zero
// width joiner are left out.
std::string CleanText(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0xF0) {
      i += 4;
      continue;
    }
    if (s.compare(i, 3, "\xEF\xB8\x8F") == 0 || s.compare(i, 3, "\xE2\x80\x8D") == 0) {
      i += 3;
      continue;
    }
    out += s[i++];
  }
  return out;
}

bool Fetchable(const std::string& url) {
  return !url.empty() && ws::IsAllowedImageHost(url) && !ws::IsBadgeOrVector(url);
}

// ------------------------------------------------------------------ worker ---
struct Job {
  bool project = false;
  int index = -1;
  std::string url;
  int max_width = 0;
};

struct Result {
  enum Kind { kFacts, kReadme, kTile, kImage } kind = kFacts;
  int project = -1;
  bool ok = false;
  bool stale = false;  // a cached copy shown because GitHub could not be reached
  ws::RepoFacts facts;
  std::vector<md::Block> blocks;
  std::string url;
  ws::Picture pic;
};

struct Shared {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<Job> jobs;
  std::deque<Result> results;
  std::atomic<bool> stop{false};
  bool exited = false;
  std::condition_variable exited_cv;
  fs::path dir;
  bool offline = false;
  int64_t max_age = ws::kRefreshSeconds;
};

void Post(Shared& sh, Result r) {
  std::lock_guard lock(sh.mutex);
  sh.results.push_back(std::move(r));
}

std::string How(const ws::CacheResult& c) {
  if (!c.ok) return "failed (HTTP " + std::to_string(c.status) + ")";
  if (!c.asked) return "cache";
  if (c.status == 200) return "downloaded";
  if (c.status == 304) return "not modified (cache kept)";
  return "saved copy (HTTP " + std::to_string(c.status) + ")";
}

bool Stale(const ws::CacheResult& c) { return c.ok && c.asked && c.status != 200 && c.status != 304; }

void LoadProject(Shared& sh, int index, const ws::Fetcher& fetch) {
  const ws::Project& p = ws::kProjects[index];
  const auto start = std::chrono::steady_clock::now();
  auto get = [&](const std::string& url, const ws::Headers& headers, size_t max) {
    return ws::CachedGet(sh.dir, url, headers, max, Now(), sh.max_age, fetch);
  };
  const ws::Headers api = {{"Accept", "application/vnd.github+json"}, {"X-GitHub-Api-Version", "2022-11-28"}};
  const ws::CacheResult facts_reply = get(ws::ApiRepoUrl(p), api, ws::kMaxTextBytes);
  Result facts;
  facts.kind = Result::kFacts;
  facts.project = index;
  if (facts_reply.ok) facts.facts = ws::ParseRepoFacts(std::string(facts_reply.bytes.begin(), facts_reply.bytes.end()));
  facts.ok = facts.facts.ok;
  const std::string branch = facts.facts.default_branch;
  Post(sh, std::move(facts));
  if (sh.stop) return;

  ws::CacheResult readme = get(ws::ApiReadmeUrl(p),
                               {{"Accept", "application/vnd.github.raw"}, {"X-GitHub-Api-Version", "2022-11-28"}},
                               ws::kMaxTextBytes);
  if (!readme.ok && !sh.stop) readme = get(ws::RawReadmeUrl(p, branch), {}, ws::kMaxTextBytes);
  Result doc;
  doc.kind = Result::kReadme;
  doc.project = index;
  std::string picture_url;
  if (readme.ok) {
    const std::string text(readme.bytes.begin(), readme.bytes.end());
    doc.blocks = md::Parse(text, [&](const std::string& url, bool picture) { return ws::ResolveUrl(p, branch, url, picture); });
    picture_url = ws::FirstReadmeImage(p, branch, text);
    doc.ok = true;
    doc.stale = Stale(readme);
  }
  const size_t block_count = doc.blocks.size();
  Post(sh, std::move(doc));
  if (sh.stop) return;

  // The tile's picture: the README's first picture, else the repository's social preview.
  std::string source = "README";
  ws::CacheResult page;
  if (picture_url.empty()) {
    page = get(ws::RepoUrl(p), {{"Accept", "text/html"}}, ws::kMaxTextBytes);
    if (page.ok) {
      const std::string og = ws::ResolveUrl(p, branch, md::DecodeEntities(ws::OgImage(std::string(page.bytes.begin(), page.bytes.end()))), true);
      if (Fetchable(og)) picture_url = og, source = "social preview";
    }
    if (picture_url.empty()) picture_url = ws::SocialPreviewUrl(p), source = "GitHub card";
  }
  ws::CacheResult image = get(picture_url, {}, ws::kMaxImageBytes);
  ws::Picture pic = image.ok ? ws::DecodePicture(image.bytes, 960) : ws::Picture{};
  if (!pic && source == "README" && !sh.stop) {
    picture_url = ws::SocialPreviewUrl(p);
    source = "GitHub card";
    image = get(picture_url, {}, ws::kMaxImageBytes);
    if (image.ok) pic = ws::DecodePicture(image.bytes, 960);
  }
  if (sh.stop) return;
  const int pw = pic.width, ph = pic.height;
  Result tile;
  tile.kind = Result::kTile;
  tile.project = index;
  tile.ok = bool(pic);
  tile.url = picture_url;
  tile.pic = std::move(pic);
  Post(sh, std::move(tile));
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  REXLOG_INFO("KK workshop: {}/{}: facts {}, README {} ({} blocks), picture ({}) {} {}x{} {}, {} ms", p.owner, p.repo,
              How(facts_reply), How(readme), block_count, source, picture_url, pw, ph, How(image), ms);
}

void LoadImage(Shared& sh, const Job& job, const ws::Fetcher& fetch) {
  const ws::CacheResult got = ws::CachedGet(sh.dir, job.url, {}, ws::kMaxImageBytes, Now(), sh.max_age, fetch);
  ws::Picture pic = got.ok ? ws::DecodePicture(got.bytes, job.max_width) : ws::Picture{};
  if (sh.stop) return;
  REXLOG_INFO("KK workshop: README picture {} {}x{} {}", job.url, pic.width, pic.height, How(got));
  Result r;
  r.kind = Result::kImage;
  r.project = job.index;
  r.url = job.url;
  r.ok = bool(pic);
  r.pic = std::move(pic);
  Post(sh, std::move(r));
}

void RunWorker(std::shared_ptr<Shared> sh) {
  ws::Fetcher fetch;
  if (sh->offline) fetch = [](const ws::FetchRequest&) { return ws::FetchReply{}; };
  else fetch = [raw = sh.get()](const ws::FetchRequest& r) { return ws::HttpFetch(r, &raw->stop); };
  for (;;) {
    Job job;
    {
      std::unique_lock lock(sh->mutex);
      sh->cv.wait(lock, [&] { return sh->stop.load() || !sh->jobs.empty(); });
      if (sh->stop) break;
      job = std::move(sh->jobs.front());
      sh->jobs.pop_front();
    }
    if (job.project) LoadProject(*sh, job.index, fetch);
    else LoadImage(*sh, job, fetch);
  }
  std::lock_guard lock(sh->mutex);
  sh->exited = true;
  sh->exited_cv.notify_all();
}

// ------------------------------------------------------------------ layout ---
// The README laid out for a width: text runs, pictures, rules and panels, link targets (in document
// coordinates, from 0,0).
struct Run {
  ImFont* font = nullptr;
  float size = 0;
  ImU32 col = 0;
  ImVec2 pos;
  std::string text;
  int link = -1;
};
struct Pic {
  std::string url;  // "" for a placeholder that is never fetched
  std::string alt;
  ImVec2 p0, p1;
  int link = -1;
};
enum class DecoKind { kHairline, kCodePanel, kQuoteBar, kTableLine };
struct Deco {
  DecoKind kind;
  ImVec2 p0, p1;
};
struct Target {
  std::string url;
  std::vector<std::pair<ImVec2, ImVec2>> rects;
  bool picture = false;  // a linked picture (its frame shows the focus)
};
struct Doc {
  int project = -1;
  float width = 0, scale = 0;
  int version = -1;
  float height = 0;
  std::vector<Run> runs;
  std::vector<Pic> pics;
  std::vector<Deco> decos;
  std::vector<Target> targets;
};

struct TextStyle {
  ImFont* font;
  ImFont* bold;
  float size;
  ImU32 col;
  float line = 1.42f;
};

class Flow {
 public:
  explicit Flow(Doc& doc) : doc_(doc) {}

  // Spans wrapped between left and right from y; returns the y below the last line.
  float Spans(const std::vector<md::Span>& spans, float left, float right, float y, const TextStyle& st) {
    const float lh = std::round(st.size * st.line);
    float x = left;
    bool line_has = false;
    bool space = false;
    std::string last_link;
    int target = -1;
    for (const md::Span& sp : spans) {
      if (sp.text == "\n") {
        x = left;
        y += lh;
        line_has = false;
        space = false;
        continue;
      }
      ImFont* font = sp.bold ? st.bold : st.font;
      ImU32 col = st.col;
      if (sp.code) col = color::kAccent;
      else if (sp.italic) col = ui::WithAlpha(color::kText, 0.78f);
      if (!sp.link.empty()) {
        col = color::kAccent;
        if (sp.link != last_link || target < 0) {
          doc_.targets.push_back({sp.link, {}});
          target = int(doc_.targets.size()) - 1;
          last_link = sp.link;
        }
      } else {
        target = -1;
        last_link.clear();
      }
      const std::string text = CleanText(sp.text);
      const float space_w = font->CalcTextSizeA(st.size, FLT_MAX, 0, " ").x;
      size_t i = 0;
      while (i < text.size()) {
        if (text[i] == ' ' || text[i] == '\n' || text[i] == '\t') {
          space = true;
          ++i;
          continue;
        }
        size_t end = i;
        while (end < text.size() && text[end] != ' ' && text[end] != '\n' && text[end] != '\t') ++end;
        std::string word = text.substr(i, end - i);
        i = end;
        float ww = font->CalcTextSizeA(st.size, FLT_MAX, 0, word.c_str(), word.c_str() + word.size()).x;
        // A word longer than the line (an address): split where it fits.
        while (ww > right - left && word.size() > 1) {
          size_t fit = 1;
          for (size_t k = 1; k <= word.size(); ++k) {
            if (k < word.size() && (static_cast<unsigned char>(word[k]) & 0xC0) == 0x80) continue;
            if (font->CalcTextSizeA(st.size, FLT_MAX, 0, word.c_str(), word.c_str() + k).x > right - left - (x - left))
              break;
            fit = k;
          }
          if (line_has && fit <= 1) {
            x = left;
            y += lh;
            line_has = false;
            continue;
          }
          Place(font, st.size, col, word.substr(0, fit), x, y, lh, target, false);
          word = word.substr(fit);
          x = left;
          y += lh;
          line_has = false;
          space = false;
          ww = font->CalcTextSizeA(st.size, FLT_MAX, 0, word.c_str(), word.c_str() + word.size()).x;
        }
        float sw = space && line_has ? space_w : 0.0f;
        if (line_has && x + sw + ww > right) {
          x = left;
          y += lh;
          line_has = false;
          sw = 0;
        }
        Place(font, st.size, col, word, x + sw, y, lh, target, sw > 0);
        x += sw + ww;
        line_has = true;
        space = false;
      }
    }
    return y + lh;
  }

 private:
  void Place(ImFont* font, float size, ImU32 col, const std::string& word, float x, float y, float lh, int target,
             bool spaced) {
    const float w = font->CalcTextSizeA(size, FLT_MAX, 0, word.c_str(), word.c_str() + word.size()).x;
    Run* prev = doc_.runs.empty() ? nullptr : &doc_.runs.back();
    if (prev && prev->font == font && prev->size == size && prev->col == col && prev->link == target &&
        prev->pos.y == y && spaced) {
      prev->text += " " + word;
    } else if (prev && prev->font == font && prev->size == size && prev->col == col && prev->link == target &&
               prev->pos.y == y && !spaced &&
               std::abs(prev->pos.x + font->CalcTextSizeA(size, FLT_MAX, 0, prev->text.c_str()).x - x) < 0.5f) {
      prev->text += word;
    } else {
      doc_.runs.push_back({font, size, col, ImVec2(x, y), word, target});
    }
    if (target >= 0) {
      auto& rects = doc_.targets[size_t(target)].rects;
      const float x0 = spaced ? x - font->CalcTextSizeA(size, FLT_MAX, 0, " ").x : x;
      if (!rects.empty() && rects.back().first.y == y) rects.back().second.x = x + w;
      else rects.push_back({ImVec2(x0, y), ImVec2(x + w, y + lh)});
    }
  }

  Doc& doc_;
};

}  // namespace

// --------------------------------------------------------------------- page ---
struct Workshop::Impl {
  enum class Load { kPending, kReady, kFailed };
  struct Texture {
    std::unique_ptr<rex::ui::ImmediateTexture> tex;
    float aspect = 16.0f / 9.0f;
    int width = 0;
  };
  struct ProjectState {
    bool facts_ok = false;
    ws::RepoFacts facts;
    Load readme = Load::kPending;
    bool readme_stale = false;
    std::vector<md::Block> blocks;
    Load tile = Load::kPending;
    Texture picture;
    bool images_queued = false;
    float lift = 0;
  };
  struct WebImage {
    Load state = Load::kPending;
    Texture t;
  };

  rex::ui::ImmediateDrawer* drawer;
  fs::path dir;
  fs::path user_dir;
  std::shared_ptr<Shared> shared;
  std::thread worker;
  bool started = false;
  bool no_browser = false;

  std::array<ProjectState, ws::kProjectCount> projects;
  std::map<std::string, WebImage> images;
  int image_version = 0;

  // Task 209: the page's three rows (kRowTextures, kRowMods, kRowProjects), each with its own focused card and
  // scroll; `row` has the focus. `tile` is the project shown (the projects row's card).
  enum { kRowTextures = 0, kRowMods = 1, kRowProjects = 2, kRows = 3 };
  int row = 0, tile = 0;
  // Task 221: the projects row's order (project indexes), shuffled each time the Workshop opens (OnOpen).
  std::vector<int> order = ws::ShuffledOrder(ws::kProjectCount, [](int k) { return k - 1; });  // in order until opened
  int card[kRows] = {1, 0, 0};  // the texture packs row starts on the HD texture pack (after the original textures)
  float row_scroll[kRows] = {}, row_target[kRows] = {};
  std::vector<float> lift[kRows];
  WorkshopHost host;
  std::unique_ptr<TexturePackShelf> shelf;
  // The modding guide (MODDING.md beside the game), shown like a README.
  bool guide = false;
  ProjectState guide_page;
  float scroll_x = 0, scroll_x_target = 0;
  // A project's page (-1: the tiles).
  int detail = -1;
  float scroll_y = 0, scroll_y_target = 0, view_h = 0, max_scroll = 0;
  int focus = 0;  // 0 Download, 1 View on GitHub, 2 + k: link target k
  Doc doc;
  // Where things were drawn this frame (mouse).
  ImVec2 button_rect[2][2]{};

  std::unique_ptr<KongMods> kong;  // row 1 (task 200)

  Impl(rex::ui::ImmediateDrawer* d, fs::path user, fs::path config_path)
      : drawer(d), dir(Workshop::CacheDir(user)), user_dir(user), kong(std::make_unique<KongMods>(d, user, config_path)) {
    kong->SetFilter(KongMods::Filter::kOthers);  // the mods row's mod pages: LB / RB go through the other mods
    shelf = std::make_unique<TexturePackShelf>(user, kong.get());
    shelf->SetHost(&host);
#if defined(KK_DEV_TOOLS)
    no_browser = std::getenv("KK_DEV_WORKSHOP_NO_BROWSER") != nullptr;
#endif
  }

  ~Impl() {
    if (!started) return;
    {
      std::lock_guard lock(shared->mutex);
      shared->stop = true;
      shared->jobs.clear();
    }
    shared->cv.notify_all();
    // A request in flight ends within its timeouts; the launcher does not wait for more than a moment.
    std::unique_lock lock(shared->mutex);
    const bool exited = shared->exited_cv.wait_for(lock, std::chrono::milliseconds(1500), [&] { return shared->exited; });
    lock.unlock();
    if (exited) worker.join();
    else worker.detach();
  }

  void Start() {
    if (started) return;
    started = true;
    shared = std::make_shared<Shared>();
    shared->dir = dir;
#if defined(KK_DEV_TOOLS)
    // Testing aids: no network at all (as offline), and the age after which the cache is asked again.
    shared->offline = std::getenv("KK_DEV_WORKSHOP_OFFLINE") != nullptr;
    if (const char* age = std::getenv("KK_DEV_WORKSHOP_MAX_AGE")) shared->max_age = std::atoll(age);
#endif
    {
      std::lock_guard lock(shared->mutex);
      for (int i = 0; i < ws::kProjectCount; ++i) shared->jobs.push_back({true, i, {}, 0});
    }
    worker = std::thread(RunWorker, shared);
    REXLOG_INFO("KK workshop: started ({} projects, cache {}{})", ws::kProjectCount, dir.string(),
                shared->offline ? ", offline test" : "");
  }

  // Task 221: a fresh random order for the projects row, its focus back on the first card and the row scrolled
  // to its start (each time the Workshop page opens; a project's page and back keeps it).
  void Shuffle() {
    static std::mt19937 rng(std::random_device{}() ^ uint32_t(std::chrono::steady_clock::now().time_since_epoch().count()));
    order = ws::ShuffledOrder(ws::kProjectCount, [](int k) { return int(std::uniform_int_distribution<int>(0, k - 1)(rng)); });
    card[kRowProjects] = 0;
    row_scroll[kRowProjects] = row_target[kRowProjects] = 0;
    lift[kRowProjects].clear();
    std::string names;
    for (const int i : order) names += (names.empty() ? "" : " | ") + std::string(ws::kProjects[size_t(i)].name);
    REXLOG_INFO("KK workshop: opened; projects order {}", names);
  }

  void Queue(Job job, bool front) {
    {
      std::lock_guard lock(shared->mutex);
      if (front) shared->jobs.push_front(std::move(job));
      else shared->jobs.push_back(std::move(job));
    }
    shared->cv.notify_one();
  }

  // The README's pictures, once its page is open (the first kMaxReadmeImages fetchable ones).
  void QueueReadmeImages(int p) {
    ProjectState& ps = projects[size_t(p)];
    if (ps.images_queued || ps.readme != Load::kReady) return;
    ps.images_queued = true;
    std::vector<std::string> urls;
    for (const md::Block& b : ps.blocks)
      for (const md::Image& im : b.images)
        if (Fetchable(im.src) && std::find(urls.begin(), urls.end(), im.src) == urls.end() &&
            int(urls.size()) < ws::kMaxReadmeImages)
          urls.push_back(im.src);
    // In reverse to the front: the first picture is fetched first, before the other projects' work.
    for (auto it = urls.rbegin(); it != urls.rend(); ++it) {
      if (images.count(*it)) continue;
      images[*it];
      Queue({false, p, *it, 1280}, true);
    }
  }

  std::unique_ptr<rex::ui::ImmediateTexture> MakeTexture(const ws::Picture& pic) {
    if (!pic || !drawer) return nullptr;
    return drawer->CreateTexture(uint32_t(pic.width), uint32_t(pic.height), rex::ui::ImmediateTextureFilter::kLinear,
                                 false, pic.rgba.data());
  }

  // Results from the worker; at most two pictures become textures a frame.
  void TakeResults() {
    if (!started) return;
    int uploads = 0;
    for (;;) {
      Result r;
      {
        std::lock_guard lock(shared->mutex);
        if (shared->results.empty()) return;
        const Result& next = shared->results.front();
        if ((next.kind == Result::kTile || next.kind == Result::kImage) && next.ok && uploads >= 2) return;
        r = std::move(shared->results.front());
        shared->results.pop_front();
      }
      ProjectState* ps = r.project >= 0 && r.project < ws::kProjectCount ? &projects[size_t(r.project)] : nullptr;
      switch (r.kind) {
        case Result::kFacts:
          if (ps) ps->facts_ok = r.ok, ps->facts = std::move(r.facts);
          break;
        case Result::kReadme:
          if (!ps) break;
          ps->readme = r.ok ? Load::kReady : Load::kFailed;
          ps->readme_stale = r.stale;
          ps->blocks = std::move(r.blocks);
          if (r.project == detail) QueueReadmeImages(detail);
          ++image_version;
          break;
        case Result::kTile:
          if (!ps) break;
          if (r.ok) {
            ps->picture.tex = MakeTexture(r.pic);
            ps->picture.aspect = float(r.pic.width) / float(r.pic.height);
            ps->picture.width = r.pic.width;
            ++uploads;
          }
          ps->tile = ps->picture.tex ? Load::kReady : Load::kFailed;
          break;
        case Result::kImage: {
          WebImage& im = images[r.url];
          if (r.ok) {
            im.t.tex = MakeTexture(r.pic);
            im.t.aspect = float(r.pic.width) / float(r.pic.height);
            im.t.width = r.pic.width;
            ++uploads;
          }
          im.state = im.t.tex ? Load::kReady : Load::kFailed;
          ++image_version;
          break;
        }
      }
    }
  }

  void OpenLink(const std::string& url, const char* what) {
    if (url.rfind("https://", 0) != 0) {
      ui::Cue(ui::Sound::kDeny);
      return;
    }
    ui::Cue(ui::Sound::kSelect);
    if (no_browser) {
      REXLOG_INFO("KK workshop: {} {} (browser not opened: KK_DEV_WORKSHOP_NO_BROWSER)", what, url);
      return;
    }
    REXLOG_INFO("KK workshop: {} {}", what, url);
    OpenUrl(url);
  }

  void OpenDetail(int p) {
    detail = std::clamp(p, 0, ws::kProjectCount - 1);
    tile = ws::PositionOf(order, detail);
    scroll_y = scroll_y_target = 0;
    focus = 0;
    doc = Doc{};
    REXLOG_INFO("KK workshop: open {}", ws::kProjects[detail].repo);
    QueueReadmeImages(detail);
  }

  // ------------------------------------------------------------- tiles ---
  void DrawCaption(ImDrawList* dl, const ui::Fonts& f, float x0, float x1, float y, float s, const char* label) {
    const float fs = 13 * s;
    const std::string caption = text::Upper(T(label));
    const float w = ui::TrackedWidth(f.display, fs, caption, 2.2f * s);
    ui::DrawTracked(dl, f.display, fs, ImVec2(x0 + 2 * s, y), color::kAccent, caption, 2.2f * s);
    dl->AddLine(ImVec2(x0 + w + 16 * s, std::round(y + fs * 0.55f)), ImVec2(x1, std::round(y + fs * 0.55f)),
                color::kHairline, 1.0f);
  }

  // A picture filling a rectangle, cropped to its shape. A much wider (a banner) or taller picture is shown
  // whole instead, over a dimmed, cropped copy of itself.
  static void DrawCover(ImDrawList* dl, rex::ui::ImmediateTexture* tex, float aspect, ImVec2 a, ImVec2 b, ImU32 tint) {
    const float view = (b.x - a.x) / std::max(1.0f, b.y - a.y);
    if (aspect > view * 1.3f || aspect < view * 0.75f) {
      DrawCoverCrop(dl, tex, aspect, view, a, b, ui::WithAlpha(IM_COL32(70, 72, 78, 255), 1.0f));
      ImVec2 c0 = a, c1 = b;
      if (aspect > view) {
        const float h = (b.x - a.x) / aspect, y = std::round((a.y + b.y - h) * 0.5f);
        c0.y = y;
        c1.y = y + h;
      } else {
        const float w = (b.y - a.y) * aspect, x = std::round((a.x + b.x - w) * 0.5f);
        c0.x = x;
        c1.x = x + w;
      }
      dl->AddImage(ui::Tex(tex), c0, c1, ImVec2(0, 0), ImVec2(1, 1), tint);
      return;
    }
    DrawCoverCrop(dl, tex, aspect, view, a, b, tint);
  }

  static void DrawCoverCrop(ImDrawList* dl, rex::ui::ImmediateTexture* tex, float aspect, float view, ImVec2 a,
                            ImVec2 b, ImU32 tint) {
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (aspect > view) {
      const float u = view / aspect;
      u0 = (1 - u) * 0.5f;
      u1 = u0 + u;
    } else {
      const float v = aspect / view;
      v0 = (1 - v) * 0.5f;
      v1 = v0 + v;
    }
    dl->AddImage(ui::Tex(tex), a, b, ImVec2(u0, v0), ImVec2(u1, v1), tint);
  }

  // A picture's place before it is there (or when it can't be): a plain panel with the name.
  static void DrawPlainPanel(ImDrawList* dl, const ui::Fonts& f, ImVec2 a, ImVec2 b, float s, const std::string& name,
                             bool loading) {
    dl->AddRectFilled(a, b, ui::WithAlpha(color::kPanel, 0.86f));
    dl->AddRectFilledMultiColor(a, b, ui::WithAlpha(color::kAccent, 0.12f), ui::WithAlpha(color::kAccent, 0.03f),
                                ui::WithAlpha(color::kBase, 0.0f), ui::WithAlpha(color::kAccent, 0.06f));
    if (loading) {
      const float t = float(ImGui::GetTime());
      const float k = 0.5f + 0.5f * std::sin(t * 3.0f);
      dl->AddRectFilled(a, b, ui::WithAlpha(color::kText, 0.025f + 0.03f * k));
    }
    if (name.empty()) return;
    const float fs = std::min(22 * s, (b.y - a.y) * 0.16f), wrap = (b.x - a.x) - 36 * s;
    const ImVec2 size = f.display->CalcTextSizeA(fs, FLT_MAX, wrap, name.c_str());
    dl->AddText(f.display, fs, ImVec2(a.x + 18 * s, (a.y + b.y - size.y) * 0.5f), ui::WithAlpha(color::kText, 0.85f),
                name.c_str(), nullptr, wrap);
  }

  std::string FactsLine(const ProjectState& ps) const {
    if (!ps.facts_ok) return {};
    std::vector<std::string> parts;
    if (ps.facts.stars >= 0) parts.push_back(F("{0} stars", {std::to_string(ps.facts.stars)}));
    if (!ps.facts.pushed.empty()) parts.push_back(F("Updated {0}", {ps.facts.pushed}));
    if (!ps.facts.license.empty()) parts.push_back(ps.facts.license);
    std::string out;
    for (const std::string& p : parts) out += (out.empty() ? "" : "   " KK_WS_DOT "   ") + p;
    return out;
  }

  // -------------------------------------------------------------- the page ---
  // Task 209 (owner, 18 Oct): one page, three rows of cards (texture packs, mods, projects). Each card is drawn the
  // same way: a picture (or a plain panel), a band with its name and a line under it, a badge for its state.
  // Action cards (Import texture pack, Import mod, Modding guide, Open mods folder, Build your own mods and texture
  // packs) are plain panels with their label.
  struct RowCard {
    std::string key, name, sub, hint, state;
    rex::ui::ImmediateTexture* picture = nullptr;
    float aspect = 16.0f / 9.0f;
    ImU32 state_bg = 0, state_fg = 0;
    std::string corner;  // a second badge at the top left (Built-in, Conflict)
    ImU32 corner_bg = 0;
    bool action = false, loading = false;
    float progress = -1;
  };

  static float Badge(ImDrawList* dl, const ui::Fonts& f, float x, float y, float s, const std::string& label, ImU32 bg,
                     ImU32 fg, bool right) {
    const float fs = 11.5f * s;
    const std::string up = text::Upper(label);
    const float w = ui::TrackedWidth(f.display, fs, up, 1.5f * s) + 12 * s;
    if (right) x -= w;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + fs + 8 * s), bg, 2 * s);
    ui::DrawTracked(dl, f.display, fs, ImVec2(x + 6 * s, y + 4 * s), fg, up, 1.5f * s);
    return w;
  }

  // One row: its caption (and, while it has the focus, the focused card's hint at the right), then its cards.
  // Returns the card clicked or chosen with A (-1: none).
  int DrawRow(ImDrawList* dl, int r, const char* caption, const std::vector<RowCard>& cards, float x0, float x1, float y,
              float tile_w, float pic_h, float band_h, const ui::Metrics& m, ui::Input& in, bool active) {
    const float s = m.s;
    const ui::Fonts f = ui::GetFonts();
    const ImGuiIO& io = ImGui::GetIO();
    const int n = int(cards.size());
    const bool focused_row = row == r;
    card[r] = std::clamp(card[r], 0, std::max(0, n - 1));
    if (lift[r].size() != size_t(n)) lift[r].assign(size_t(n), 0.0f);
    const float gap = 16 * s, avail = x1 - x0;
    const float tile_h = pic_h + band_h, top = y + 26 * s;
    const ImVec2 r0(x0 - 14 * s, top - 8 * s), r1(x1 + 14 * s, top + tile_h + 6 * s);
    const float content_w = n * tile_w + std::max(0, n - 1) * gap, max_x = std::max(0.0f, content_w - avail);
    const int before = card[r];
    int open = -1;
    if (active && focused_row) {
      if (in.Pressed(Action::kLeft) || in.Pressed(Action::kPrevTab)) card[r] = std::max(0, card[r] - 1);
      if (in.Pressed(Action::kRight) || in.Pressed(Action::kNextTab)) card[r] = std::min(n - 1, card[r] + 1);
      in.Consume(Action::kPrevTab);
      in.Consume(Action::kNextTab);
    }
    if (active) {
      const float wheel = io.MouseWheel + io.MouseWheelH;
      if (wheel != 0 && ImGui::IsMouseHoveringRect(r0, r1, false))
        row_target[r] = std::clamp(row_target[r] - wheel * (tile_w + gap) * 0.5f, 0.0f, max_x);
    }
    if (card[r] != before || (focused_row && n > 0)) {
      // Keep the focused card in view.
      const float tx = card[r] * (tile_w + gap);
      if (tx < row_target[r]) row_target[r] = tx;
      if (tx + tile_w > row_target[r] + avail) row_target[r] = tx + tile_w - avail;
    }
    if (card[r] != before) ui::Cue(ui::Sound::kMove);
    row_target[r] = std::clamp(row_target[r], 0.0f, max_x);
    row_scroll[r] = ui::Approach(row_scroll[r], row_target[r], 14.0f);

    // Caption, and the focused card's hint.
    const float cfs = 13 * s;
    const std::string cap = text::Upper(T(caption));
    const float cw = ui::TrackedWidth(f.display, cfs, cap, 2.2f * s);
    ui::DrawTracked(dl, f.display, cfs, ImVec2(x0 + 2 * s, y), focused_row ? color::kAccent : ui::WithAlpha(color::kAccent, 0.7f),
                    cap, 2.2f * s);
    float line_x = x0 + cw + 16 * s;
    if (focused_row && n > 0 && !cards[size_t(card[r])].hint.empty()) {
      const float hfs = 13.5f * s;
      const std::string hint = ui::Ellipsize(f.text, hfs, cards[size_t(card[r])].hint, std::max(40 * s, x1 - line_x - 8 * s));
      const float hw = ui::TextSize(f.text, hfs, hint).x;
      dl->AddText(f.text, hfs, ImVec2(x1 - hw, y - 1 * s), color::kTextDim, hint.c_str());
      dl->AddLine(ImVec2(line_x, std::round(y + cfs * 0.55f)), ImVec2(x1 - hw - 14 * s, std::round(y + cfs * 0.55f)),
                  color::kHairline, 1.0f);
    } else {
      dl->AddLine(ImVec2(line_x, std::round(y + cfs * 0.55f)), ImVec2(x1, std::round(y + cfs * 0.55f)), color::kHairline, 1.0f);
    }

    dl->PushClipRect(r0, r1, true);
    for (int k = 0; k < n; ++k) {
      const RowCard& c = cards[size_t(k)];
      const float x = std::round(x0 + k * (tile_w + gap) - row_scroll[r]);
      if (x > r1.x || x + tile_w < r0.x) continue;
      const bool on_card = focused_row && card[r] == k;
      lift[r][size_t(k)] = ui::Approach(lift[r][size_t(k)], on_card ? 100.0f : 0.0f, 12.0f);
      const float cy = std::round(top - 5 * s * lift[r][size_t(k)] / 100.0f);
      const ImVec2 a(x, cy), b(x + tile_w, cy + tile_h), pb(x + tile_w, cy + pic_h);
      if (active && ImGui::IsMouseHoveringRect(ImVec2(std::max(a.x, x0 - 14 * s), a.y), ImVec2(std::min(b.x, x1 + 14 * s), b.y), false)) {
        if (in.mouse_moved()) {
          if (row != r || card[r] != k) ui::Cue(ui::Sound::kMove);
          row = r, card[r] = k;
        }
        if (ImGui::IsMouseClicked(0)) row = r, card[r] = k, open = k;
      }
      dl->AddRectFilled(a, b, ui::WithAlpha(color::kPanel, 0.88f));
      if (c.action) {
        // A button card: a soft brass panel, a plus or arrow mark, the label.
        dl->AddRectFilledMultiColor(a, pb, ui::WithAlpha(color::kAccent, on_card ? 0.30f : 0.18f),
                                    ui::WithAlpha(color::kAccent, 0.06f), ui::WithAlpha(color::kBase, 0.0f),
                                    ui::WithAlpha(color::kAccent, 0.10f));
        const float mfs = std::min(40 * s, pic_h * 0.42f);
        const char* mark = c.key == "link" ? "\xE2\x86\x97" : c.key == "guide" ? "?" : c.key == "folder" ? "\xE2\x80\xA6" : "+";
        const ImVec2 ms = f.display->CalcTextSizeA(mfs, FLT_MAX, 0, mark);
        dl->AddText(f.display, mfs, ImVec2(a.x + (tile_w - ms.x) * 0.5f, a.y + (pic_h - ms.y) * 0.5f),
                    on_card ? color::kText : color::kTextDim, mark);
      } else if (c.picture) {
        DrawCover(dl, c.picture, c.aspect, a, pb, on_card ? IM_COL32_WHITE : IM_COL32(205, 208, 214, 255));
      } else {
        DrawPlainPanel(dl, f, a, pb, s, c.loading ? std::string() : c.name, c.loading);
      }
      if (!c.state.empty()) Badge(dl, f, b.x - 8 * s, a.y + 8 * s, s, c.state, c.state_bg, c.state_fg, true);
      if (!c.corner.empty()) Badge(dl, f, a.x + 8 * s, pb.y - 27 * s, s, c.corner, c.corner_bg, color::kOnFocus, false);
      if (c.progress >= 0) {
        dl->AddRectFilled(ImVec2(a.x, pb.y - 4 * s), ImVec2(b.x, pb.y), ui::WithAlpha(color::kBase, 0.8f));
        dl->AddRectFilled(ImVec2(a.x, pb.y - 4 * s), ImVec2(a.x + tile_w * c.progress, pb.y), color::kAccent);
      }
      const float nfs = 15 * s;
      dl->AddText(f.semibold, nfs, ImVec2(x + 12 * s, pb.y + 7 * s), on_card ? color::kText : color::kTextDim,
                  ui::Ellipsize(f.semibold, nfs, c.name, tile_w - 24 * s).c_str());
      dl->AddText(f.text, 12.5f * s, ImVec2(x + 12 * s, pb.y + 27 * s), color::kTextFaint,
                  ui::Ellipsize(f.text, 12.5f * s, c.sub, tile_w - 24 * s).c_str());
      if (on_card) {
        dl->AddRect(ImVec2(a.x - 1 * s, a.y - 1 * s), ImVec2(b.x + 1 * s, b.y + 1 * s), color::kFocus, 0, 0, 2 * s);
        dl->AddRectFilled(ImVec2(a.x, pb.y), ImVec2(a.x + 3 * s, b.y), color::kAccent);
      } else {
        dl->AddRect(a, b, ui::WithAlpha(color::kText, 0.08f));
      }
    }
    dl->PopClipRect();
    if (row_scroll[r] < max_x - 1)
      dl->AddRectFilledMultiColor(ImVec2(x1 - 40 * s, r0.y), ImVec2(x1 + 14 * s, r1.y), ui::WithAlpha(color::kBase, 0.0f),
                                  ui::WithAlpha(color::kBase, 0.75f), ui::WithAlpha(color::kBase, 0.75f),
                                  ui::WithAlpha(color::kBase, 0.0f));
    if (row_scroll[r] > 1)
      dl->AddRectFilledMultiColor(ImVec2(x0 - 14 * s, r0.y), ImVec2(x0 + 26 * s, r1.y), ui::WithAlpha(color::kBase, 0.75f),
                                  ui::WithAlpha(color::kBase, 0.0f), ui::WithAlpha(color::kBase, 0.0f),
                                  ui::WithAlpha(color::kBase, 0.75f));
    if (active && focused_row && n > 0 && in.Pressed(Action::kAccept)) {
      in.Consume(Action::kAccept);
      open = card[r];
    }
    return open;
  }

  std::vector<RowCard> TextureCards() {
    std::vector<RowCard> out;
    for (auto& c : shelf->Cards()) {
      RowCard rc;
      rc.key = c.key;
      rc.name = c.name;
      rc.sub = c.sub;
      rc.hint = c.hint;
      rc.picture = c.picture;
      rc.aspect = c.aspect;
      rc.state = c.state;
      rc.state_bg = c.state_bg;
      rc.state_fg = c.state_fg;
      rc.action = c.action;
      rc.progress = c.progress;
      out.push_back(std::move(rc));
    }
    return out;
  }

  std::vector<RowCard> ModCards() {
    std::vector<RowCard> out;
    const bool scanned = kong->Scanned();
    for (const auto& m : kong->Cards()) {
      if (m.texture_pack) continue;  // in the texture packs row
      RowCard rc;
      rc.key = "mod:" + m.id;
      rc.name = m.name;
      rc.sub = m.author;
      if (!m.version.empty()) rc.sub += (rc.sub.empty() ? "" : "   " KK_WS_DOT "   ") + F("Version {0}", {m.version});
      rc.picture = m.picture;
      rc.aspect = m.aspect;
      if (m.on) {
        rc.state = F("Enabled {0}", {std::to_string(m.position + 1)});
        rc.state_bg = ui::WithAlpha(color::kGood, 0.95f);
        rc.state_fg = color::kOnFocus;
      } else if (!m.usable) {
        rc.state = T("Problem");
        rc.state_bg = ui::WithAlpha(color::kBad, 0.92f);
        rc.state_fg = color::kOnFocus;
      } else {
        rc.state = T("Disabled");
        rc.state_bg = ui::WithAlpha(color::kBase, 0.82f);
        rc.state_fg = color::kTextDim;
      }
      if (m.builtin) rc.corner = T("Built-in"), rc.corner_bg = ui::WithAlpha(color::kAccent, 0.92f);
      else if (m.conflict) rc.corner = T("Conflict"), rc.corner_bg = ui::WithAlpha(color::kWarn, 0.92f);
      std::string d = m.description.substr(0, m.description.find('\n'));
      rc.hint = rc.state + (d.empty() ? std::string() : "   " KK_WS_DOT "   " + d);
      out.push_back(std::move(rc));
    }
    if (!scanned) {
      RowCard rc;
      rc.key = "scanning";
      rc.name = T("Looking for mods");
      rc.loading = true;
      out.push_back(std::move(rc));
    }
    auto action = [&](const char* key, const char* name, const char* sub, const char* hint) {
      RowCard rc;
      rc.key = key;
      rc.action = true;
      rc.name = T(name);
      rc.sub = T(sub);
      rc.hint = T(hint);
      out.push_back(std::move(rc));
    };
    action("import", "Import mod", "From a ZIP file",
           "A ZIP with one folder holding mod.toml (a settings or cutscenes mod) and its files, as MODDING.md describes. "
           "It is checked before anything is installed.");
    action("guide", "Modding guide", "MODDING.md", "How to make texture packs, settings mods and cutscene mods for the game.");
    action("folder", "Open mods folder", "Your mods", "Opens the folder your mods go in, so you can add or remove them.");
    action("link", "Build your own mods and texture packs", "kong.tekrantgaming.com/mods",
           "Opens the Mods page of the game's website in your browser, with the guides and the modding tools to download.");
    return out;
  }

  std::vector<RowCard> ProjectCards() {
    std::vector<RowCard> out;
    for (const int i : order) {  // task 221: shuffled each time the Workshop opens
      const ProjectState& ps = projects[size_t(i)];
      RowCard rc;
      rc.key = std::to_string(i);
      rc.name = ws::kProjects[i].name;
      rc.sub = std::string("github.com/") + ws::kProjects[i].owner;
      if (ps.tile == Load::kReady && ps.picture.tex) rc.picture = ps.picture.tex.get(), rc.aspect = ps.picture.aspect;
      rc.loading = ps.tile == Load::kPending;
      std::string desc = ps.facts_ok ? CleanText(ps.facts.description) : std::string();
      if (desc.empty() && ps.readme == Load::kFailed && ps.tile == Load::kFailed)
        desc = T("Could not reach GitHub. Check your internet connection.");
      const std::string facts = FactsLine(ps);
      rc.hint = desc.empty() ? facts : facts.empty() ? desc : desc + "   " KK_WS_DOT "   " + facts;
      out.push_back(std::move(rc));
    }
    return out;
  }

  static constexpr const char* kModsUrl = "https://kong.tekrantgaming.com/mods";

  void DrawPage(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ui::Metrics& m, ui::Input& in, bool active) {
    const float s = m.s;
    const int row_before = row;
    if (active) {
      if (in.Pressed(Action::kUp) && row > 0) --row;
      if (in.Pressed(Action::kDown) && row < kRows - 1) ++row;
    }
    if (row != row_before) ui::Cue(ui::Sound::kMove);
    // Three rows share the height: picture 16:9, a band of two lines, the caption and a gap.
    const float row_h = (p1.y - p0.y) / 3.0f, band_h = 46 * s;
    const float pic_h = std::round(std::clamp(row_h - band_h - 26 * s - 18 * s, 60 * s, 200 * s));
    const float tile_w = std::round(pic_h * 16.0f / 9.0f);
    const std::vector<RowCard> rows[kRows] = {TextureCards(), ModCards(), ProjectCards()};
    const char* captions[kRows] = {"Texture packs", "Mods", "Other TRG projects and games"};
    for (int r = 0; r < kRows; ++r) {
      const int open = DrawRow(dl, r, captions[r], rows[r], p0.x, p1.x, std::round(p0.y + r * row_h), tile_w, pic_h,
                               band_h, m, in, active);
      if (open < 0 || open >= int(rows[r].size())) continue;
      const std::string& key = rows[r][size_t(open)].key;
      if (r == kRowTextures) {
        ui::Cue(ui::Sound::kSelect);
        shelf->Activate(key);
      } else if (r == kRowMods) {
        if (key == "scanning") ui::Cue(ui::Sound::kDeny);
        else if (key == "import") {
          ui::Cue(ui::Sound::kSelect);
          if (host.import_zip) host.import_zip(content::ImportTarget::kMod);
        } else if (key == "guide") {
          ui::Cue(ui::Sound::kSelect);
          OpenGuide();
        } else if (key == "folder") {
          OpenModsFolder();
        } else if (key == "link") {
          ui::Cue(ui::Sound::kSelect);
          OpenLink(kModsUrl, "build your own mods:");
        } else if (key.rfind("mod:", 0) == 0) {
          ui::Cue(ui::Sound::kSelect);
          kong->OpenMod(key.substr(4));
        }
      } else {
        ui::Cue(ui::Sound::kSelect);
        OpenDetail(std::atoi(key.c_str()));
      }
    }
    tile = card[kRowProjects];
  }

  // -------------------------------------------------------------- Mods ---
  // Workshop > Mods: the Kong mods row (task 200; texture pack mods are in Texture packs), then the modding
  // guide and the mods folder. Down / Up move between the row and the two buttons, Left / Right between them.
  void OpenGuide() {
    guide = true;
    focus = 0;
    scroll_y = scroll_y_target = 0;
    doc = Doc{};
    const fs::path path = mods::BuiltinDir().parent_path() / "MODDING.md";
    std::ifstream file(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(file)), {});
    guide_page = ProjectState{};
    guide_page.tile = Load::kFailed;
    // Only web addresses open (the guide's own anchors and relative links do not), and no pictures.
    guide_page.blocks = md::Parse(text, [](const std::string& url, bool picture) {
      return !picture && url.rfind("https://", 0) == 0 ? url : std::string();
    });
    guide_page.readme = text.empty() ? Load::kFailed : Load::kReady;
    ++image_version;
    REXLOG_INFO("KK workshop: modding guide {} ({} bytes, {} blocks)", path.string(), text.size(), guide_page.blocks.size());
  }

  void OpenModsFolder() {
    const fs::path folder = mods::UserDir(user_dir);
    ui::Cue(ui::Sound::kSelect);
    if (no_browser) {
      REXLOG_INFO("KK workshop: mods folder {} (not opened: KK_DEV_WORKSHOP_NO_BROWSER)", folder.string());
      return;
    }
    REXLOG_INFO("KK workshop: opening the mods folder {}", folder.string());
    OpenInExplorer(folder);
  }

  // ------------------------------------------------------------ README ---
  // A README picture's state: one that is not fetched (a badge, another host, past the limit) counts as
  // failed (a linked one shows as its link).
  const WebImage* Image(const std::string& url) const {
    if (!Fetchable(url)) return nullptr;
    const auto it = images.find(url);
    return it == images.end() ? nullptr : &it->second;
  }

  float LayoutImages(Flow& flow, const md::Block& b, float width, float y, float s, const TextStyle& body) {
    const float indent = b.level * 20 * s, col = width - indent;
    std::vector<md::Span> chips;
    struct Item {
      const md::Image* im;
      float w, aspect;
    };
    std::vector<Item> items;
    for (const md::Image& im : b.images) {
      const WebImage* web = Image(im.src);
      if (!web || web->state == Load::kFailed) {
        if (!im.link.empty()) {
          if (!chips.empty()) chips.push_back(md::Span{"  " KK_WS_DOT "  "});
          chips.push_back(md::Span{im.alt.empty() ? std::string(T("Link")) : im.alt, false, false, false, im.link});
        }
        continue;
      }
      const bool ready = web->state == Load::kReady;
      const float aspect = ready ? web->t.aspect : 16.0f / 9.0f;
      // GitHub shows a README about 880 pixels wide: sizes given in pixels keep that share of the column.
      float w = col;
      if (im.width_pct > 0) w = col * std::min(im.width_pct, 100) / 100.0f;
      else if (im.width_px > 0) w = std::min(col, im.width_px * col / 880.0f);
      else if (ready) w = std::min(col, web->t.width * col / 880.0f);
      items.push_back({&im, std::max(w, 24 * s), aspect});
    }
    if (!chips.empty()) y = flow.Spans(chips, indent, width, y, body) + 6 * s;
    if (items.empty()) return y;
    const float gap = 10 * s;
    float sum = gap * float(items.size() - 1);
    for (const Item& it : items) sum += it.w;
    const float fixed = gap * float(items.size() - 1);
    const float k = sum > col ? (col - fixed) / std::max(1.0f, sum - fixed) : 1.0f;
    float x = indent + (col - std::min(sum, col)) * 0.5f;
    float row_h = 0;
    for (const Item& it : items) {
      const float w = std::floor(it.w * k), h = std::floor(w / it.aspect);
      Pic pic{it.im->src, it.im->alt, ImVec2(x, y), ImVec2(x + w, y + h), -1};
      if (!it.im->link.empty()) {
        doc.targets.push_back({it.im->link, {{pic.p0, pic.p1}}, true});
        pic.link = int(doc.targets.size()) - 1;
      }
      doc.pics.push_back(std::move(pic));
      x += w + gap;
      row_h = std::max(row_h, h);
    }
    return y + row_h + 12 * s;
  }

  // `p`: the project, or kGuideDoc for the modding guide.
  static constexpr int kGuideDoc = -2;
  void BuildDoc(int p, const std::vector<md::Block>& blocks, float width, float s) {
    doc = Doc{};
    doc.project = p;
    doc.width = width;
    doc.scale = s;
    doc.version = image_version;
    const ui::Fonts f = ui::GetFonts();
    Flow flow(doc);
    const TextStyle body{f.text, f.semibold, 15.5f * s, ui::WithAlpha(color::kText, 0.92f)};
    float y = 0;
    bool first = true;
    for (const md::Block& b : blocks) {
      switch (b.kind) {
        case md::BlockKind::kHeading: {
          const int level = b.level;
          TextStyle st = level <= 1   ? TextStyle{f.display, f.display, 25 * s, color::kText, 1.25f}
                         : level == 2 ? TextStyle{f.display, f.display, 21 * s, color::kText, 1.25f}
                         : level == 3 ? TextStyle{f.semibold, f.semibold, 18 * s, color::kText, 1.3f}
                                      : TextStyle{f.semibold, f.semibold, 16 * s, color::kText, 1.3f};
          if (!first) y += (level <= 2 ? 22 : 14) * s;
          y = flow.Spans(b.spans, 0, width, y, st);
          if (level <= 2) {
            y += 4 * s;
            doc.decos.push_back({DecoKind::kHairline, ImVec2(0, y), ImVec2(width, y + 1)});
            y += 10 * s;
          } else {
            y += 6 * s;
          }
          break;
        }
        case md::BlockKind::kParagraph:
          y = flow.Spans(b.spans, 0, width, y, body) + 10 * s;
          break;
        case md::BlockKind::kListItem: {
          const float indent = (b.level * 20 + 22) * s;
          if (b.number >= 0) {
            const std::string marker = b.number == 0 ? "\xE2\x80\xA2" : std::to_string(b.number) + ".";
            const float mw = f.text->CalcTextSizeA(body.size, FLT_MAX, 0, marker.c_str()).x;
            doc.runs.push_back({f.text, body.size, color::kAccent, ImVec2(indent - 8 * s - mw, y), marker, -1});
          }
          y = flow.Spans(b.spans, indent, width, y, body) + 4 * s;
          break;
        }
        case md::BlockKind::kQuote: {
          const float y0 = y;
          TextStyle st = body;
          st.col = color::kTextDim;
          y = flow.Spans(b.spans, 18 * s, width, y, st);
          doc.decos.push_back({DecoKind::kQuoteBar, ImVec2(0, y0), ImVec2(3 * s, y)});
          y += 10 * s;
          break;
        }
        case md::BlockKind::kCode: {
          const float fs = 14 * s, lh = std::round(fs * 1.4f), y0 = y;
          y += 10 * s;
          size_t start = 0;
          const std::string code = CleanText(b.code);
          while (start <= code.size()) {
            size_t end = code.find('\n', start);
            if (end == std::string::npos) end = code.size();
            const std::string line = code.substr(start, end - start);
            if (!line.empty()) doc.runs.push_back({f.text, fs, color::kTextDim, ImVec2(14 * s, y), line, -1});
            y += lh;
            start = end + 1;
          }
          y += 8 * s;
          doc.decos.push_back({DecoKind::kCodePanel, ImVec2(0, y0), ImVec2(width, y)});
          y += 12 * s;
          break;
        }
        case md::BlockKind::kRule:
          y += 8 * s;
          doc.decos.push_back({DecoKind::kHairline, ImVec2(0, y), ImVec2(width, y + 1)});
          y += 14 * s;
          break;
        case md::BlockKind::kTableRow: {
          const int cols = std::max<int>(1, int(b.cells.size()));
          const float cw = width / float(cols);
          TextStyle st = body;
          st.size = 14.5f * s;
          if (b.header) st.font = f.semibold;
          float bottom = y + 6 * s;
          for (int c = 0; c < int(b.cells.size()); ++c)
            bottom = std::max(bottom, flow.Spans(b.cells[size_t(c)], c * cw + 6 * s, (c + 1) * cw - 8 * s, y + 6 * s, st));
          y = bottom + 4 * s;
          doc.decos.push_back({b.header ? DecoKind::kHairline : DecoKind::kTableLine, ImVec2(0, y), ImVec2(width, y + 1)});
          y += 2 * s;
          break;
        }
        case md::BlockKind::kImages:
          y = LayoutImages(flow, b, width, y, s, body);
          break;
      }
      first = false;
    }
    doc.height = y;
  }

  void Activate(int target) {
    if (guide) {
      if (target == 0) OpenModsFolder();
      else if (target - 2 >= 0 && target - 2 < int(doc.targets.size())) OpenLink(doc.targets[size_t(target - 2)].url, "link:");
      return;
    }
    const ws::Project& p = ws::kProjects[detail];
    if (target == 0) OpenLink(ws::ReleasesUrl(p), "download: opening the latest release page");
    else if (target == 1) OpenLink(ws::RepoUrl(p), "view on GitHub:");
    else if (target - 2 < int(doc.targets.size())) OpenLink(doc.targets[size_t(target - 2)].url, "link:");
  }

  bool TargetVisible(int k) const {
    if (k < 0 || k >= int(doc.targets.size()) || doc.targets[size_t(k)].rects.empty()) return false;
    const auto& r = doc.targets[size_t(k)].rects;
    return r.front().first.y - scroll_y >= -2 && r.back().second.y - scroll_y <= view_h + 2;
  }

  void DrawDetail(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ui::Metrics& m, ui::Input& in, bool active) {
    const float s = m.s;
    const ui::Fonts f = ui::GetFonts();
    const ImGuiIO& io = ImGui::GetIO();
    if (active && !guide && (in.Pressed(Action::kPrevTab) || in.Pressed(Action::kNextTab))) {
      const int dir = in.Pressed(Action::kNextTab) ? 1 : -1;
      in.Consume(Action::kPrevTab);
      in.Consume(Action::kNextTab);
      ui::Cue(ui::Sound::kTab);
      const int at = ws::PositionOf(order, detail);  // the next project in the row's order
      OpenDetail(order[size_t((at + dir + ws::kProjectCount) % ws::kProjectCount)]);
    }
    // The modding guide uses the same page: its text instead of a README, one button (Open mods folder).
    const int p = guide ? kGuideDoc : detail;
    ProjectState& ps = guide ? guide_page : projects[size_t(p)];
    const int buttons = guide ? 1 : 2;
    const std::string name = guide ? std::string(T("Modding guide")) : std::string(ws::kProjects[p].name);
    const float avail = p1.x - p0.x;
    const float col_w = std::round(avail * 0.60f);
    const ImVec2 c0(p0.x, p0.y + 4 * s), c1(p0.x + col_w, p1.y);
    const float text_w = col_w - 24 * s;
    view_h = c1.y - c0.y;
    if (ps.readme == Load::kReady &&
        (doc.project != p || doc.width != text_w || doc.scale != s || doc.version != image_version))
      BuildDoc(p, ps.blocks, text_w, s);
    const bool ready = ps.readme == Load::kReady && doc.project == p;
    max_scroll = ready ? std::max(0.0f, doc.height - view_h + 24 * s) : 0.0f;
    if (active) {
      if (in.Pressed(Action::kUp)) scroll_y_target -= 70 * s;
      if (in.Pressed(Action::kDown)) scroll_y_target += 70 * s;
      if (in.Pressed(Action::kPageUp)) scroll_y_target -= view_h * 0.85f;
      if (in.Pressed(Action::kPageDown)) scroll_y_target += view_h * 0.85f;
      if (io.MouseWheel != 0 && ImGui::IsMouseHoveringRect(c0, c1, false)) scroll_y_target -= io.MouseWheel * 96 * s;
    }
    scroll_y_target = std::clamp(scroll_y_target, 0.0f, max_scroll);
    scroll_y = ui::Approach(scroll_y, scroll_y_target, 16.0f);
    if (std::abs(scroll_y - scroll_y_target) < 0.5f) scroll_y = scroll_y_target;

    // Focus: the links on screen, then the two buttons (left/right go round them).
    if (focus >= 2 && !TargetVisible(focus - 2)) focus = 0;
    if (active && (in.Pressed(Action::kLeft) || in.Pressed(Action::kRight))) {
      std::vector<int> order;
      for (int k = 0; k < int(doc.targets.size()); ++k)
        if (TargetVisible(k)) order.push_back(k + 2);
      for (int b = 0; b < buttons; ++b) order.push_back(b);
      const int at = int(std::find(order.begin(), order.end(), focus) - order.begin());
      const int n = int(order.size());
      focus = order[size_t((at + (in.Pressed(Action::kRight) ? 1 : n - 1)) % n)];
      ui::Cue(ui::Sound::kMove);
    }
    // The mouse on a link.
    int hover = -1;
    if (active && ready && ImGui::IsMouseHoveringRect(c0, c1, false)) {
      const ImVec2 mp = ImGui::GetMousePos();
      const ImVec2 at(mp.x - c0.x, mp.y - c0.y + scroll_y);
      for (int k = 0; k < int(doc.targets.size()) && hover < 0; ++k)
        for (const auto& r : doc.targets[size_t(k)].rects)
          if (at.x >= r.first.x && at.x <= r.second.x && at.y >= r.first.y && at.y <= r.second.y) {
            hover = k;
            break;
          }
      if (hover >= 0 && in.mouse_moved()) focus = hover + 2;
      if (hover >= 0 && ImGui::IsMouseClicked(0)) {
        focus = hover + 2;
        Activate(focus);
      }
    }

    // The README.
    if (ready) {
      dl->PushClipRect(c0, c1, true);
      const ImVec2 o(c0.x, c0.y - std::round(scroll_y));
      auto visible = [&](float y0, float y1) { return o.y + y1 >= c0.y && o.y + y0 <= c1.y; };
      for (const Deco& d : doc.decos) {
        if (!visible(d.p0.y, d.p1.y)) continue;
        const ImVec2 a(o.x + d.p0.x, o.y + d.p0.y), b(o.x + d.p1.x, o.y + d.p1.y);
        switch (d.kind) {
          case DecoKind::kHairline: dl->AddRectFilled(a, ImVec2(b.x, a.y + 1), ui::WithAlpha(color::kText, 0.14f)); break;
          case DecoKind::kTableLine: dl->AddRectFilled(a, ImVec2(b.x, a.y + 1), color::kHairline); break;
          case DecoKind::kQuoteBar: dl->AddRectFilled(a, b, ui::WithAlpha(color::kAccent, 0.7f)); break;
          case DecoKind::kCodePanel:
            dl->AddRectFilled(a, b, ui::WithAlpha(color::kBase, 0.72f));
            dl->AddRect(a, b, color::kHairline);
            break;
        }
      }
      for (const Pic& pic : doc.pics) {
        if (!visible(pic.p0.y, pic.p1.y)) continue;
        const ImVec2 a(o.x + pic.p0.x, o.y + pic.p0.y), b(o.x + pic.p1.x, o.y + pic.p1.y);
        const WebImage* web = Image(pic.url);
        if (web && web->state == Load::kReady && web->t.tex) dl->AddImage(ui::Tex(web->t.tex.get()), a, b);
        else DrawPlainPanel(dl, f, a, b, s, std::string(), true);
        if (pic.link >= 0 && (focus == pic.link + 2 || hover == pic.link))
          dl->AddRect(ImVec2(a.x - 2 * s, a.y - 2 * s), ImVec2(b.x + 2 * s, b.y + 2 * s), color::kFocus, 0, 0, 2 * s);
      }
      for (const Run& r : doc.runs) {
        if (!visible(r.pos.y, r.pos.y + r.size * 1.5f)) continue;
        const bool lit = r.link >= 0 && (focus == r.link + 2 || hover == r.link);
        dl->AddText(r.font, r.size, ImVec2(o.x + r.pos.x, o.y + r.pos.y), lit ? color::kFocus : r.col, r.text.c_str());
      }
      // The focused / hovered link: underlined, on a faint band.
      for (const int k : {focus - 2, hover}) {
        if (k < 0 || k >= int(doc.targets.size()) || doc.targets[size_t(k)].picture) continue;
        for (const auto& [ra, rb] : doc.targets[size_t(k)].rects) {
          const ImVec2 a(o.x + ra.x - 3 * s, o.y + ra.y), b(o.x + rb.x + 3 * s, o.y + rb.y);
          dl->AddRectFilled(a, b, ui::WithAlpha(color::kFocus, k == focus - 2 ? 0.12f : 0.06f), 3 * s);
          dl->AddLine(ImVec2(a.x + 3 * s, b.y - 3 * s), ImVec2(b.x - 3 * s, b.y - 3 * s), color::kFocus, 1.0f);
        }
      }
      dl->PopClipRect();
      // The scroll bar.
      if (max_scroll > 0) {
        const float track = c1.y - c0.y, thumb = std::max(40 * s, track * view_h / (doc.height + 24 * s));
        const float ty = c0.y + (track - thumb) * (scroll_y / max_scroll);
        dl->AddRectFilled(ImVec2(c1.x - 3 * s, c0.y), ImVec2(c1.x, c1.y), color::kHairline);
        dl->AddRectFilled(ImVec2(c1.x - 3 * s, ty), ImVec2(c1.x, ty + thumb), ui::WithAlpha(color::kAccent, 0.8f));
      }
    } else if (ps.readme == Load::kPending) {
      const int dots = int(ImGui::GetTime() * 3) % 4;
      const std::string loading = std::string(T("Loading")) + std::string(size_t(dots), '.');
      dl->AddText(f.text, 16 * s, ImVec2(c0.x, c0.y + 8 * s), color::kTextDim, loading.c_str());
    } else {
      dl->AddText(f.text, 16 * s, ImVec2(c0.x, c0.y + 8 * s), color::kTextDim,
                  guide ? T("The modding guide (MODDING.md) was not found beside the game.")
                        : T("Could not load this page. Check your internet connection and try again later."),
                  nullptr, text_w);
    }

    // The side panel: picture, name, description, facts and the two buttons.
    const float x0 = c1.x + 56 * s, x1 = p1.x, pw = x1 - x0;
    float y = p0.y + 8 * s;
    const float pic_h = std::round(std::min(pw * 9.0f / 16.0f, (p1.y - p0.y) * 0.34f)), pic_w = std::round(pic_h * 16.0f / 9.0f);
    const ImVec2 pa(x0, y), pb(x0 + pic_w, y + pic_h);
    if (ps.tile == Load::kReady && ps.picture.tex) DrawCover(dl, ps.picture.tex.get(), ps.picture.aspect, pa, pb, IM_COL32_WHITE);
    else DrawPlainPanel(dl, f, pa, pb, s, ps.tile == Load::kFailed ? name : std::string(), ps.tile == Load::kPending);
    dl->AddRect(pa, pb, ui::WithAlpha(color::kText, 0.10f));
    dl->AddRectFilled(pa, ImVec2(pa.x + 3 * s, pb.y), color::kAccent);
    y = pb.y + 16 * s;
    dl->AddText(f.display, 25 * s, ImVec2(x0, y), color::kText, name.c_str(), nullptr, pw);
    y += f.display->CalcTextSizeA(25 * s, FLT_MAX, pw, name.c_str()).y + 4 * s;
    const std::string repo = guide ? std::string("MODDING.md")
                                   : std::string("github.com/") + ws::kProjects[p].owner + "/" + ws::kProjects[p].repo;
    dl->AddText(f.text, 13.5f * s, ImVec2(x0, y), color::kTextFaint, ui::Ellipsize(f.text, 13.5f * s, repo, pw).c_str());
    y += 24 * s;
    if (guide) {
      const char* desc = T("How to make texture packs, settings mods and cutscene mods for the game.");
      dl->AddText(f.text, 15 * s, ImVec2(x0, y), color::kTextDim, desc, nullptr, pw);
      y += f.text->CalcTextSizeA(15 * s, FLT_MAX, pw, desc).y + 8 * s;
    }
    if (ps.facts_ok && !ps.facts.description.empty()) {
      // At most three lines.
      const std::string desc = CleanText(ps.facts.description);
      const float dfs = 15 * s, lh = dfs * 1.18f;
      const float h = std::min(f.text->CalcTextSizeA(dfs, FLT_MAX, pw, desc.c_str()).y, lh * 3);
      dl->PushClipRect(ImVec2(x0, y), ImVec2(x1, y + h + 1), true);
      dl->AddText(f.text, dfs, ImVec2(x0, y), color::kTextDim, desc.c_str(), nullptr, pw);
      dl->PopClipRect();
      y += h + 8 * s;
    }
    if (const std::string facts = FactsLine(ps); !facts.empty()) {
      dl->AddText(f.text, 13.5f * s, ImVec2(x0, y), color::kTextFaint, ui::Ellipsize(f.text, 13.5f * s, facts, pw).c_str());
      y += 24 * s;
    }
    y += 6 * s;
    const char* labels[2] = {guide ? "Open mods folder" : "Download", "View on GitHub"};
    const float bh = 44 * s;
    for (int i = 0; i < buttons; ++i) {
      const ImVec2 a(x0, y), b(x1, y + bh);
      button_rect[i][0] = a;
      button_rect[i][1] = b;
      const bool hot = active && ImGui::IsMouseHoveringRect(a, b, false);
      if (hot && in.mouse_moved()) focus = i;
      if (hot && ImGui::IsMouseClicked(0)) {
        focus = i;
        Activate(i);
      }
      const bool on = focus == i;
      if (on) dl->AddRectFilled(a, b, color::kFocus);
      else {
        dl->AddRectFilled(a, b, ui::WithAlpha(color::kPanel, 0.85f));
        dl->AddRect(a, b, ui::WithAlpha(color::kText, 0.14f));
      }
      const std::string label = text::Upper(T(labels[i]));
      const float lfs = 15 * s, tw = ui::TrackedWidth(f.display, lfs, label, 2 * s);
      ui::DrawTracked(dl, f.display, lfs, ImVec2(a.x + (pw - tw) * 0.5f, a.y + (bh - lfs) * 0.5f),
                      on ? color::kOnFocus : color::kText, label, 2 * s);
      y += bh + 10 * s;
    }
    const char* hint = guide       ? "Opens the folder your mods go in, so you can add or remove them."
                       : focus == 1 ? "Opens the project's page on GitHub in your web browser."
                                    : "Opens the latest release on GitHub in your web browser. The launcher does not "
                                      "download or run anything.";
    dl->AddText(f.text, 13.5f * s, ImVec2(x0, y), color::kTextFaint, T(hint), nullptr, pw);
    y += f.text->CalcTextSizeA(13.5f * s, FLT_MAX, pw, T(hint)).y + 10 * s;
    if (ps.readme_stale && y < p1.y - 20 * s)
      dl->AddText(f.text, 13.5f * s, ImVec2(x0, y), color::kWarn,
                  T("Showing the saved copy: GitHub could not be reached."), nullptr, pw);

    if (active && in.Pressed(Action::kAccept)) {
      in.Consume(Action::kAccept);
      Activate(focus);
    }
  }

  void Draw(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ui::Metrics& m, ui::Input& in, bool active) {
    Start();
    TakeResults();
    shelf->Tick();
    if (detail >= 0 || guide) DrawDetail(dl, p0, p1, m, in, active);
    else if (shelf->InDetail()) shelf->DrawDetail(dl, p0, p1, m, in, active);
    else if (kong->InDetail()) kong->DrawDetail(dl, p0, p1, m, in, active);
    else DrawPage(dl, p0, p1, m, in, active);
  }

  void Dev(const std::string& c) {
    auto load = [](Load l) { return l == Load::kReady ? "ready" : l == Load::kFailed ? "failed" : "pending"; };
    if (c == "state") {
      for (int i = 0; i < ws::kProjectCount; ++i) {
        const ProjectState& ps = projects[size_t(i)];
        REXLOG_INFO("KK workshop: state {} facts {} readme {}{} ({} blocks) picture {} stars {} updated {}",
                    ws::kProjects[i].repo, ps.facts_ok ? "ready" : "none", load(ps.readme),
                    ps.readme_stale ? " (saved copy)" : "", ps.blocks.size(), load(ps.tile), ps.facts.stars,
                    ps.facts.pushed);
      }
      int ready = 0, failed = 0, pending = 0;
      for (const auto& [url, im] : images)
        (im.state == Load::kReady ? ready : im.state == Load::kFailed ? failed : pending)++;
      REXLOG_INFO("KK workshop: state view {} row {} tile {} focus {} scroll {:.0f}/{:.0f} doc {:.0f} px, {} runs, {} "
                  "pictures, {} links; README pictures ready {} failed {} pending {}",
                  detail >= 0 ? ws::kProjects[detail].repo : "tiles", row, tile, focus, scroll_y, max_scroll,
                  doc.height, doc.runs.size(), doc.pics.size(), doc.targets.size(), ready, failed, pending);
    } else if (c == "order") {  // task 221: the projects row's order
      std::string names;
      for (const int i : order) names += (names.empty() ? "" : " | ") + std::string(ws::kProjects[size_t(i)].name);
      REXLOG_INFO("KK workshop: projects order {}", names);
    } else if (c.rfind("row:", 0) == 0) {  // task 209: the focused row (0 texture packs, 1 mods, 2 projects)
      row = std::clamp(std::atoi(c.c_str() + 4), 0, kRows - 1);
    } else if (c.rfind("card:", 0) == 0) {  // the focused row's card n
      card[row] = std::max(0, std::atoi(c.c_str() + 5));
    } else if (c == "cards") {
      const std::vector<RowCard> rows[kRows] = {TextureCards(), ModCards(), ProjectCards()};
      for (int r = 0; r < kRows; ++r)
        for (size_t k = 0; k < rows[r].size(); ++k)
          REXLOG_INFO("KK workshop: row {} card {} {} '{}' [{}]{}", r, k, rows[r][k].key, rows[r][k].name, rows[r][k].state,
                      row == r && card[r] == int(k) ? " (focused)" : "");
    } else if (c.rfind("tex:", 0) == 0) {
      shelf->Dev(c.substr(4));
    } else if (c.rfind("open:", 0) == 0) {
      OpenDetail(std::atoi(c.c_str() + 5));
    } else if (c.rfind("tile:", 0) == 0) {
      detail = -1;
      row = kRowProjects;
      card[row] = tile = std::clamp(std::atoi(c.c_str() + 5), 0, ws::kProjectCount - 1);
    } else if (c == "mods") {
      guide = false;
      row = kRowMods;
    } else if (c.rfind("mods:", 0) == 0) {
      guide = false;
      row = kRowMods;
      kong->Dev(c.substr(5));
    } else if (c == "guide") {
      OpenGuide();
    } else if (c == "folder") {
      OpenModsFolder();
    } else if (detail >= 0 && c == "download") {
      Activate(0);
    } else if (detail >= 0 && c == "github") {
      Activate(1);
    } else if (detail >= 0 && c.rfind("link:", 0) == 0) {
      const int k = std::atoi(c.c_str() + 5);
      if (k >= 0 && k < int(doc.targets.size())) Activate(k + 2);
    } else if (c == "links") {
      for (size_t k = 0; k < doc.targets.size() && k < 40; ++k)
        REXLOG_INFO("KK workshop: link {} {}", k, doc.targets[k].url);
    } else if (c.rfind("scroll:", 0) == 0) {
      scroll_y_target += float(std::atof(c.c_str() + 7));
    }
  }
};

Workshop::Workshop(rex::ui::ImmediateDrawer* drawer, fs::path user_dir, fs::path config_path)
    : impl_(std::make_unique<Impl>(drawer, std::move(user_dir), std::move(config_path))) {}

Workshop::~Workshop() = default;

void Workshop::SetHost(WorkshopHost host) {
  impl_->host = std::move(host);
  impl_->kong->SetArt(impl_->host.art);  // task 221: built-in mods' pictures
}
void Workshop::OnOpen() { impl_->Shuffle(); }
bool Workshop::InDetail() const {
  return impl_->detail >= 0 || impl_->guide || impl_->kong->InDetail() || impl_->shelf->InDetail();
}

void Workshop::Draw(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ui::Metrics& m, ui::Input& in, bool active) {
  impl_->Draw(dl, p0, p1, m, in, active);
}

bool Workshop::Back() {
  if (impl_->guide) {
    impl_->guide = false;
    return true;
  }
  if (impl_->detail >= 0) {
    impl_->card[Impl::kRowProjects] = ws::PositionOf(impl_->order, impl_->detail);  // back on the project shown (LB / RB may have moved on)
    impl_->detail = -1;
    return true;
  }
  if (impl_->shelf->Back()) return true;
  return impl_->kong->Back();
}

std::vector<ui::Prompt> Workshop::Prompts() const {
  if (impl_->detail >= 0 || impl_->guide) return {{Action::kUp, "Scroll"}, {Action::kAccept, "Select"}, {Action::kBack, "Back"}};
  if (impl_->kong->InDetail()) return impl_->kong->Prompts(false);
  return {{Action::kAccept, "Select"}, {Action::kBack, "Back"}};
}

std::string Workshop::DetailTitle() const {
  if (impl_->guide) return T("Modding guide");
  if (impl_->detail >= 0) return T("Other TRG projects and games");
  if (impl_->shelf->InDetail()) return T("Texture packs");
  if (impl_->kong->InDetail()) return T("Mods");
  return {};
}

KongMods& Workshop::Mods() { return *impl_->kong; }
std::vector<std::pair<std::string, std::string>> Workshop::TexturePackChoices() { return impl_->shelf->Choices(); }
std::string Workshop::ActiveTexturePack() { return impl_->shelf->Active(); }
void Workshop::EnableTexturePack(const std::string& key) { impl_->shelf->Enable(key); }
void Workshop::OnTexturePackModInstalled(const std::string& id) { impl_->shelf->OnModInstalled(id); }
void Workshop::OnModsChanged(const std::string& removed_id) {
  if (!removed_id.empty()) impl_->shelf->OnModRemoved(removed_id);
  else impl_->kong->Rescan();
}
void Workshop::OnFolderPackOn() { impl_->shelf->OnFolderOn(); }
void Workshop::Tick() { impl_->shelf->Tick(); }

void Workshop::DevCommand(const std::string& command) { impl_->Dev(command); }

fs::path Workshop::CacheDir(const fs::path& user_dir) { return user_dir / "cache" / "workshop"; }

}  // namespace kk
