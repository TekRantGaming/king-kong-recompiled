// This file is UTF-8.
#include "artbook.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

#include <rex/logging.h>
#include <rex/ui/immediate_drawer.h>

#include "artbook_files.h"
#include "artbook_logic.h"
#include "content.h"
#include "launcher_text.h"
#include "toast.h"
#include "workshop_net.h"

namespace kk {

namespace fs = std::filesystem;
namespace color = ui::color;
using ui::Action;
using text::F;
using text::T;

namespace {

std::string U8(const fs::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

constexpr int kThumbWidth = 800;
constexpr int kFullWidth = 4096;
constexpr float kMaxPixelZoom = 4.0f;  // at most 4 screen pixels per picture pixel

// The decoding worker: jobs in, pictures out.
struct Job {
  int index;
  bool full;
  fs::path file;
};
struct Result {
  int index;
  bool full;
  workshop::Picture picture;
};
struct Shared {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<Job> jobs;
  std::deque<Result> results;
  bool stop = false;
};

void RunWorker(std::shared_ptr<Shared> sh) {
  for (;;) {
    Job job;
    {
      std::unique_lock lock(sh->mutex);
      sh->cv.wait(lock, [&] { return sh->stop || !sh->jobs.empty(); });
      if (sh->stop) return;
      job = std::move(sh->jobs.front());
      sh->jobs.pop_front();
    }
    std::vector<uint8_t> bytes;
    if (!artbook::ReadBytes(job.file, bytes, artbook::kMaxPictureBytes)) bytes.clear();
    Result r{job.index, job.full, workshop::DecodePicture(bytes, job.full ? kFullWidth : kThumbWidth)};
    std::lock_guard lock(sh->mutex);
    if (sh->stop) return;
    sh->results.push_back(std::move(r));
  }
}

float Smooth(float v, float target, float rate) {
  const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
  return v + (target - v) * (1.0f - std::exp(-rate * dt));
}

std::string Mb(uint64_t bytes) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f", double(bytes) / 1e6);
  return buf;
}

}  // namespace

struct Artbook::Impl {
  enum class Load { kNone, kPending, kReady, kFailed };
  struct Texture {
    std::unique_ptr<rex::ui::ImmediateTexture> tex;
    Load state = Load::kNone;
  };
  struct Painting {
    fs::path file;
    std::string title;
    int width = 0, height = 0;  // the original's size (from its JPEG header)
    Texture thumb, full;
    float lift = 0;
  };

  rex::ui::ImmediateDrawer* drawer;
  fs::path folder;
  std::shared_ptr<Shared> shared = std::make_shared<Shared>();
  std::thread worker;
  bool started = false;

  bool loaded = false;
  std::vector<Painting> paintings;
  std::string readme;
  std::deque<int> full_order;  // paintings holding a full-size texture, oldest first (at most two)

  int focus = 0;
  bool button_focus = true;  // not installed: Download now
  int view = -1;             // the viewer's painting (-1: the grid)
  float zoom = 1, zoom_target = 1;
  float cx = 0, cy = 0;      // the picture point at the middle of the view (picture pixels)
  ImVec2 anchor{-1, -1};     // the screen point a stepped zoom keeps still
  bool dragging = false;
  ImVec2 drag_from{};
  float view_fade = 0;
  // Where the viewer drew this frame (the fit area) and its scale, for the mouse and dev steps.
  ImVec2 fit0{}, fit1{};
  float fit_scale = 1;
  std::string message;
  bool message_bad = false;
  double message_until = 0;

  Impl(rex::ui::ImmediateDrawer* d, const fs::path& user_dir)
      : drawer(d), folder(content::InstallDir(user_dir, content::Kind::kArtbook)) {}

  ~Impl() {
    if (started) {
      {
        std::lock_guard lock(shared->mutex);
        shared->stop = true;
        shared->jobs.clear();
      }
      shared->cv.notify_all();
      worker.join();  // a decode in progress takes a fraction of a second
    }
    for (Painting& p : paintings) {
      if (p.thumb.tex) KeepTextureAlive(std::move(p.thumb.tex));
      if (p.full.tex) KeepTextureAlive(std::move(p.full.tex));
    }
  }

  void Queue(int index, bool full) {
    if (!started) {
      started = true;
      worker = std::thread(RunWorker, shared);
    }
    Texture& t = full ? paintings[size_t(index)].full : paintings[size_t(index)].thumb;
    if (t.state != Load::kNone) return;
    t.state = Load::kPending;
    {
      std::lock_guard lock(shared->mutex);
      // The viewer's picture before the thumbnails.
      if (full) shared->jobs.push_front({index, true, paintings[size_t(index)].file});
      else shared->jobs.push_back({index, false, paintings[size_t(index)].file});
    }
    shared->cv.notify_one();
  }

  // The installed pictures, once the disc has been turned into them.
  void LoadPaintings() {
    loaded = true;
    for (const fs::path& file : artbook::Paintings(folder)) {
      Painting p;
      p.file = file;
      p.title = artbook::TitleFromFile(U8(file.filename()));
      const std::vector<uint8_t> head = artbook::ReadHead(file, 64 * 1024);
      artbook::JpegSize(head.data(), head.size(), p.width, p.height);
      if (p.width <= 0 || p.height <= 0) p.width = 3150, p.height = 1772;
      paintings.push_back(std::move(p));
    }
    readme = artbook::ReadmeText(folder);
    for (int i = 0; i < int(paintings.size()); ++i) Queue(i, false);
    focus = std::clamp(focus, 0, std::max(0, int(paintings.size()) - 1));
    REXLOG_INFO("KK artbook: {} paintings, readme {} characters, in {}", paintings.size(), readme.size(), U8(folder));
  }

  void TakeResults() {
    int uploads = 0;
    for (;;) {
      Result r;
      {
        std::lock_guard lock(shared->mutex);
        if (shared->results.empty() || uploads >= 2) return;
        r = std::move(shared->results.front());
        shared->results.pop_front();
      }
      if (r.index < 0 || r.index >= int(paintings.size())) continue;
      Painting& p = paintings[size_t(r.index)];
      Texture& t = r.full ? p.full : p.thumb;
      if (r.full && view != r.index) {  // the viewer moved on meanwhile: not kept
        t.state = Load::kNone;
        continue;
      }
      if (!r.picture || !drawer) {
        t.state = Load::kFailed;
        REXLOG_WARN("KK artbook: {} did not decode", U8(p.file.filename()));
        continue;
      }
      t.tex = drawer->CreateTexture(uint32_t(r.picture.width), uint32_t(r.picture.height),
                                    rex::ui::ImmediateTextureFilter::kLinear, false, r.picture.rgba.data());
      t.state = t.tex ? Load::kReady : Load::kFailed;
      ++uploads;
      if (r.full) {
        full_order.push_back(r.index);
        // At most two full-size pictures in memory (about 30 MB each).
        while (full_order.size() > 2) {
          Painting& old = paintings[size_t(full_order.front())];
          full_order.pop_front();
          if (old.full.tex) KeepTextureAlive(std::move(old.full.tex));
          old.full.state = Load::kNone;
        }
      }
    }
  }

  void Say(std::string text, bool bad) {
    message = std::move(text);
    message_bad = bad;
    message_until = ImGui::GetTime() + 9.0;
  }

  void Save(int index) {
    if (index < 0 || index >= int(paintings.size())) return;
    const fs::path to = artbook::SaveFolder();
    const artbook::Saved saved = artbook::SavePicture(paintings[size_t(index)].file, to);
    if (saved.ok) {
      ui::Cue(ui::Sound::kSelect);
      Say(saved.already ? F("Already saved in {0}", {U8(saved.file)}) : F("Saved to {0}", {U8(saved.file)}), false);
      REXLOG_INFO("KK artbook: saved {} -> {}{}", U8(paintings[size_t(index)].file.filename()), U8(saved.file),
                  saved.already ? " (already there)" : "");
    } else {
      ui::Cue(ui::Sound::kDeny);
      Say(F("Could not save the picture to {0}.", {U8(to)}), true);
      REXLOG_WARN("KK artbook: save failed: {}", saved.why);
    }
  }

  // ---------------------------------------------------------------- viewer ---
  void OpenViewer(int index) {
    if (index < 0 || index >= int(paintings.size())) return;
    view = index;
    zoom = zoom_target = 1;
    cx = paintings[size_t(index)].width * 0.5f;
    cy = paintings[size_t(index)].height * 0.5f;
    anchor = ImVec2(-1, -1);
    dragging = false;
    Queue(index, true);
    REXLOG_INFO("KK artbook: viewer {} ({})", index, paintings[size_t(index)].title);
  }
  void Step(int dir) {
    if (paintings.size() < 2) return;
    const int n = int(paintings.size());
    ui::Cue(ui::Sound::kMove);
    OpenViewer((view + dir + n) % n);
    focus = view;
  }
  float MaxZoom(const Painting& p) const { return std::max(2.0f, kMaxPixelZoom / std::max(fit_scale, 1e-4f)); }
  void ZoomTo(float target, ImVec2 at) {
    const Painting& p = paintings[size_t(view)];
    zoom_target = std::clamp(target, 1.0f, MaxZoom(p));
    anchor = at;
  }
  // Keeps the middle inside the picture so it never drifts off the screen.
  void ClampCenter(const Painting& p, float scale) {
    const float half_w = (fit1.x - fit0.x) * 0.5f / scale, half_h = (fit1.y - fit0.y) * 0.5f / scale;
    cx = p.width <= 2 * half_w ? p.width * 0.5f : std::clamp(cx, half_w, p.width - half_w);
    cy = p.height <= 2 * half_h ? p.height * 0.5f : std::clamp(cy, half_h, p.height - half_h);
  }

  void DrawViewer(ImDrawList* dl, ImVec2 v0, ImVec2 v1, const ui::Metrics& m, ui::Input& in, bool active) {
    const float s = m.s;
    const ui::Fonts f = ui::GetFonts();
    const ImGuiIO& io = ImGui::GetIO();
    const float dt = std::min(io.DeltaTime, 0.1f);
    Painting& p = paintings[size_t(view)];
    view_fade = Smooth(view_fade, 1.0f, 12.0f);
    fit0 = ImVec2(v0.x + 40 * s, v0.y + 78 * s);
    fit1 = ImVec2(v1.x - 40 * s, v1.y - 92 * s);
    const ImVec2 mid((fit0.x + fit1.x) * 0.5f, (fit0.y + fit1.y) * 0.5f);
    fit_scale = std::min((fit1.x - fit0.x) / float(p.width), (fit1.y - fit0.y) / float(p.height));

    // ---- input
    const bool zoomed = zoom_target > 1.02f || zoom > 1.02f;
    if (active) {
      const ui::Input::Sticks& st = in.sticks();
      // Previous / next: LB / RB (Q / E) always; Left / Right while the whole picture shows.
      if (in.Pressed(Action::kPrevTab)) Step(-1);
      else if (in.Pressed(Action::kNextTab)) Step(1);
      else if (!zoomed && in.Pressed(Action::kLeft)) Step(-1);
      else if (!zoomed && in.Pressed(Action::kRight)) Step(1);
      if (view < 0) return;
      Painting& q = paintings[size_t(view)];
      for (Action a : {Action::kPrevTab, Action::kNextTab, Action::kLeft, Action::kRight, Action::kUp, Action::kDown,
                       Action::kPageUp, Action::kPageDown})
        in.Consume(a);
      // Zoom: the triggers (analog), A / Enter / double-click (whole picture <-> 2.5x), X / Delete (whole picture),
      // the wheel (at the pointer), + / - and Page Up / Page Down (keyboard).
      if (st.rt > 0 || st.lt > 0) {
        zoom_target = std::clamp(zoom_target * std::exp((st.rt - st.lt) * dt * 2.4f), 1.0f, MaxZoom(q));
        anchor = mid;
      }
      if (in.Pressed(Action::kAccept)) {
        in.Consume(Action::kAccept);
        ui::Cue(ui::Sound::kSelect);
        ZoomTo(zoomed ? 1.0f : 2.5f, mid);
      }
      if (in.Pressed(Action::kDefault)) {
        in.Consume(Action::kDefault);
        ZoomTo(1.0f, mid);
      }
      const bool ctrl = io.KeyCtrl || io.KeyAlt;
      if (!ctrl && (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd) ||
                    ImGui::IsKeyPressed(ImGuiKey_PageUp)))
        ZoomTo(zoom_target * 1.4f, mid);
      if (!ctrl && (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract) ||
                    ImGui::IsKeyPressed(ImGuiKey_PageDown)))
        ZoomTo(zoom_target / 1.4f, mid);
      const bool over = ImGui::IsMouseHoveringRect(v0, v1, false);
      if (over && io.MouseWheel != 0) ZoomTo(zoom_target * std::pow(1.25f, io.MouseWheel), io.MousePos);
      if (over && ImGui::IsMouseDoubleClicked(0)) ZoomTo(zoomed ? 1.0f : 2.5f, io.MousePos);
      // Pan: either stick, the D-pad or the arrow keys (held), dragging with the mouse.
      const float scale_now = fit_scale * zoom;
      float px = st.lx + st.rx, py = st.ly + st.ry;
      if (st.lx == 0 && st.ly == 0) {  // the D-pad / arrow keys (the left stick also counts as those)
        if (in.HeldFor(Action::kLeft) > 0) px -= 1;
        if (in.HeldFor(Action::kRight) > 0) px += 1;
        if (in.HeldFor(Action::kUp) > 0) py -= 1;
        if (in.HeldFor(Action::kDown) > 0) py += 1;
      }
      if (px != 0 || py != 0) {
        const float speed = 900 * s * dt / scale_now;
        cx += std::clamp(px, -1.5f, 1.5f) * speed;
        cy += std::clamp(py, -1.5f, 1.5f) * speed;
      }
      if (ImGui::IsMouseClicked(0) && over) {
        dragging = true;
        drag_from = io.MousePos;
      }
      if (dragging) {
        if (!ImGui::IsMouseDown(0)) {
          // A click (not a drag) at the left or right edge: previous / next.
          const float moved = std::abs(io.MousePos.x - drag_from.x) + std::abs(io.MousePos.y - drag_from.y);
          dragging = false;
          if (moved < 4 * s && !ImGui::IsMouseDoubleClicked(0)) {
            if (io.MousePos.x < v0.x + 90 * s) Step(-1);
            else if (io.MousePos.x > v1.x - 90 * s) Step(1);
          }
        } else if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) {
          cx -= io.MouseDelta.x / scale_now;
          cy -= io.MouseDelta.y / scale_now;
        }
      }
      if (in.Pressed(Action::kResetPage)) {
        in.Consume(Action::kResetPage);
        Save(view);
      }
    }
    if (view < 0) return;
    Painting& q = paintings[size_t(view)];
    // ---- zoom animation around the anchor
    const float before = fit_scale * zoom;
    zoom = std::exp(Smooth(std::log(zoom), std::log(zoom_target), 16.0f));
    if (std::abs(zoom / zoom_target - 1.0f) < 0.002f) zoom = zoom_target;
    const float scale = fit_scale * zoom;
    if (scale != before) {
      const ImVec2 a = anchor.x >= 0 ? anchor : mid;
      const float ix = cx + (a.x - mid.x) / before, iy = cy + (a.y - mid.y) / before;
      cx = ix - (a.x - mid.x) / scale;
      cy = iy - (a.y - mid.y) / scale;
    }
    ClampCenter(q, scale);

    // ---- draw
    const float fade = view_fade;
    dl->AddRectFilled(v0, v1, ui::WithAlpha(IM_COL32(4, 5, 7, 255), 0.97f * fade));
    rex::ui::ImmediateTexture* tex = q.full.tex ? q.full.tex.get() : q.thumb.tex.get();
    const ImVec2 i0(mid.x - cx * scale, mid.y - cy * scale);
    const ImVec2 i1(i0.x + q.width * scale, i0.y + q.height * scale);
    dl->PushClipRect(v0, v1, true);
    if (tex) {
      dl->AddImage(ui::Tex(tex), i0, i1, ImVec2(0, 0), ImVec2(1, 1), ui::WithAlpha(IM_COL32_WHITE, fade));
    } else {
      dl->AddRectFilled(i0, i1, ui::WithAlpha(color::kPanel, fade));
    }
    if (!q.full.tex && q.full.state == Load::kPending) {  // the sharp one is on its way
      const std::string loading = T("Loading");
      const ImVec2 ts = ui::TextSize(f.text, 14 * s, loading);
      dl->AddText(f.text, 14 * s, ImVec2(v1.x - 40 * s - ts.x, v0.y + 46 * s), color::kTextDim, loading.c_str());
    }
    dl->PopClipRect();
    // Top: the title and where it is in the set; the zoom.
    dl->AddRectFilledMultiColor(v0, ImVec2(v1.x, v0.y + 90 * s), ui::WithAlpha(color::kBase, 0.85f * fade),
                                ui::WithAlpha(color::kBase, 0.85f * fade), ui::WithAlpha(color::kBase, 0.0f),
                                ui::WithAlpha(color::kBase, 0.0f));
    const float x = v0.x + 40 * s;
    ui::DrawTracked(dl, f.display, 12 * s, ImVec2(x + 2 * s, v0.y + 20 * s), color::kAccent,
                    text::Upper(F("{0} of {1}", {std::to_string(view + 1), std::to_string(paintings.size())})), 2.2f * s);
    dl->AddText(f.display, 26 * s, ImVec2(x, v0.y + 38 * s), color::kText, q.title.c_str());
    const int percent = int(std::lround(scale * 100.0f));
    const std::string zoom_text = F("Zoom {0}", {std::to_string(percent) + "%"});
    const ImVec2 zs = ui::TextSize(f.text, 14 * s, zoom_text);
    dl->AddText(f.text, 14 * s, ImVec2(v1.x - 40 * s - zs.x, v0.y + 24 * s), color::kTextDim, zoom_text.c_str());
    // Previous / next marks at the edges (also clickable).
    if (paintings.size() > 1) {
      const float ay = mid.y, ah = 16 * s;
      const ImU32 c = ui::WithAlpha(color::kText, 0.55f * fade);
      dl->AddTriangleFilled(ImVec2(v0.x + 24 * s, ay), ImVec2(v0.x + 36 * s, ay - ah), ImVec2(v0.x + 36 * s, ay + ah), c);
      dl->AddTriangleFilled(ImVec2(v1.x - 24 * s, ay), ImVec2(v1.x - 36 * s, ay - ah), ImVec2(v1.x - 36 * s, ay + ah), c);
    }
    DrawMessage(dl, f, s, ImVec2(v0.x + 40 * s, v1.y - 92 * s - 6 * s), v1.x - v0.x - 80 * s, true);
  }

  void DrawMessage(ImDrawList* dl, const ui::Fonts& f, float s, ImVec2 bottom_left, float width, bool panel) {
    if (message.empty() || ImGui::GetTime() > message_until) return;
    const float fs = 14.5f * s;
    const ImVec2 size = f.text->CalcTextSizeA(fs, FLT_MAX, width - 28 * s, message.c_str());
    const ImVec2 a(bottom_left.x, bottom_left.y - size.y - 20 * s), b(bottom_left.x + size.x + 28 * s, bottom_left.y);
    if (panel) dl->AddRectFilled(a, b, ui::WithAlpha(color::kPanel, 0.9f), 8 * s);
    dl->AddRectFilled(a, ImVec2(a.x + 3 * s, b.y), message_bad ? color::kBad : color::kGood);
    dl->AddText(f.text, fs, ImVec2(a.x + 14 * s, a.y + 10 * s), color::kText, message.c_str(), nullptr, width - 28 * s);
  }

  // ------------------------------------------------------------------ grid ---
  static void DrawCaption(ImDrawList* dl, const ui::Fonts& f, float x0, float x1, float y, float s,
                          const std::string& caption_upper) {
    const float fs = 13 * s;
    const float w = ui::TrackedWidth(f.display, fs, caption_upper, 2.2f * s);
    ui::DrawTracked(dl, f.display, fs, ImVec2(x0 + 2 * s, y), color::kAccent, caption_upper, 2.2f * s);
    dl->AddLine(ImVec2(x0 + w + 16 * s, std::round(y + fs * 0.55f)), ImVec2(x1, std::round(y + fs * 0.55f)),
                color::kHairline, 1.0f);
  }
  // A picture filling a rectangle, cropped to its shape.
  static void DrawCover(ImDrawList* dl, rex::ui::ImmediateTexture* tex, float aspect, ImVec2 a, ImVec2 b, ImU32 tint) {
    const float view = (b.x - a.x) / std::max(1.0f, b.y - a.y);
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

  void DrawGrid(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ui::Metrics& m, ui::Input& in, bool active) {
    const float s = m.s;
    const ui::Fonts f = ui::GetFonts();
    const int n = int(paintings.size());
    const float avail = p1.x - p0.x;
    DrawCaption(dl, f, p0.x, p1.x, p0.y + 6 * s, s, "WETA COLLECTOR'S ARTWORK");  // the disc's own name for it
    float y = p0.y + 34 * s;
    if (!readme.empty()) {
      const float rfs = 14.5f * s, wrap = std::min(avail, 1040 * s);
      const float h = std::min(f.text->CalcTextSizeA(rfs, FLT_MAX, wrap, readme.c_str()).y, rfs * 1.2f * 4);
      dl->PushClipRect(ImVec2(p0.x, y), ImVec2(p0.x + wrap, y + h + 1), true);
      dl->AddText(f.text, rfs, ImVec2(p0.x, y), color::kTextDim, readme.c_str(), nullptr, wrap);
      dl->PopClipRect();
      y += h + 18 * s;
    }
    // Three across, two down, filling what is left.
    const int cols = 3, rows = (n + cols - 1) / cols;
    const float gap = 18 * s, band = 40 * s;
    const float tile_w = std::floor((avail - gap * (cols - 1)) / cols);
    const float room = p1.y - y - 30 * s;
    const float tile_h = std::floor(std::min(tile_w * 0.56f + band, (room - gap * (rows - 1)) / std::max(rows, 1)));
    const float pic_h = std::max(20 * s, tile_h - band);
    const int before = focus;
    if (active && n > 0) {
      if (in.Pressed(Action::kLeft) && focus % cols > 0) --focus;
      if (in.Pressed(Action::kRight) && focus % cols < cols - 1 && focus + 1 < n) ++focus;
      if (in.Pressed(Action::kUp) && focus >= cols) focus -= cols;
      if (in.Pressed(Action::kDown) && focus + cols < n) focus += cols;
      for (Action a : {Action::kLeft, Action::kRight, Action::kUp, Action::kDown, Action::kPrevTab, Action::kNextTab})
        in.Consume(a);
    }
    int open = -1;
    for (int i = 0; i < n; ++i) {
      Painting& p = paintings[size_t(i)];
      const float x = std::round(p0.x + (i % cols) * (tile_w + gap));
      const bool focused = i == focus;
      p.lift = Smooth(p.lift, focused ? 1.0f : 0.0f, 12.0f);
      const float ty = std::round(y + (i / cols) * (tile_h + gap) - 4 * s * p.lift);
      const ImVec2 a(x, ty), b(x + tile_w, ty + tile_h), pb(x + tile_w, ty + pic_h);
      if (active && ImGui::IsMouseHoveringRect(a, b, false)) {
        if (in.mouse_moved()) focus = i;
        if (ImGui::IsMouseClicked(0)) focus = i, open = i;
      }
      dl->AddRectFilled(a, b, ui::WithAlpha(color::kPanel, 0.88f));
      if (p.thumb.tex) {
        DrawCover(dl, p.thumb.tex.get(), float(p.width) / float(p.height), a, pb,
                  focused ? IM_COL32_WHITE : IM_COL32(200, 203, 209, 255));
      } else {
        dl->AddRectFilledMultiColor(a, pb, ui::WithAlpha(color::kAccent, 0.12f), ui::WithAlpha(color::kAccent, 0.03f),
                                    ui::WithAlpha(color::kBase, 0.0f), ui::WithAlpha(color::kAccent, 0.06f));
      }
      dl->AddText(f.semibold, 16 * s, ImVec2(x + 14 * s, pb.y + 10 * s), focused ? color::kText : color::kTextDim,
                  ui::Ellipsize(f.semibold, 16 * s, p.title, tile_w - 28 * s).c_str());
      if (focused) {
        dl->AddRect(ImVec2(a.x - 1 * s, a.y - 1 * s), ImVec2(b.x + 1 * s, b.y + 1 * s), color::kFocus, 0, 0, 2 * s);
        dl->AddRectFilled(ImVec2(a.x, pb.y), ImVec2(a.x + 3 * s, b.y), color::kAccent);
      } else {
        dl->AddRect(a, b, ui::WithAlpha(color::kText, 0.08f));
      }
    }
    if (focus != before) ui::Cue(ui::Sound::kMove);
    if (active && in.Pressed(Action::kAccept) && n > 0) {
      in.Consume(Action::kAccept);
      open = focus;
    }
    if (active && in.Pressed(Action::kResetPage) && n > 0) {
      in.Consume(Action::kResetPage);
      Save(focus);
    }
    DrawMessage(dl, f, s, ImVec2(p0.x, p1.y + 4 * s), avail, false);
    if (open >= 0) {
      ui::Cue(ui::Sound::kSelect);
      view_fade = 0;
      OpenViewer(open);
    }
  }

  // ------------------------------------------------------ not there yet ---
  void DrawMissing(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const ui::Metrics& m, ui::Input& in, bool active,
                   const content::Status& st) {
    const float s = m.s;
    const ui::Fonts f = ui::GetFonts();
    using content::Stage;
    const bool busy = st.Busy();
    DrawCaption(dl, f, p0.x, p1.x, p0.y + 6 * s, s, text::Upper(T("Artbook")));
    const float w = std::min(p1.x - p0.x, 820 * s);
    const ImVec2 a(p0.x, p0.y + 40 * s);
    const float pad = 28 * s;
    const std::string heading =
        text::Upper(T(busy ? "The artbook is still downloading" : "The artbook is not downloaded yet"));
    const std::string body = F("The paintings come from the game's PS2 bonus disc, which the launcher downloads from "
                               "archive.org ({0} MB). Only the pictures are kept.",
                               {Mb(content::InfoFor(content::Kind::kArtbook).zip_size)});
    std::string line;
    float fraction = -1;
    if (st.stage == Stage::kQueued) line = T("Waiting for another download to finish.");
    else if (st.stage == Stage::kDownloading && st.total) {
      line = F("{0} of {1} MB", {Mb(st.done), Mb(st.total)});
      fraction = float(double(st.done) / double(st.total));
    } else if (st.stage == Stage::kStarting) line = T("Starting the download");
    else if (st.stage == Stage::kVerifying || st.stage == Stage::kUnpacking) {
      line = T("Preparing the pictures");
      if (st.total) fraction = float(double(st.done) / double(st.total));
    } else if (st.stage == Stage::kFailed) line = T("The download did not finish. Check your internet connection, then try again.");
    else if (st.stage == Stage::kPaused && st.done) line = F("{0} of {1} MB", {Mb(st.done), Mb(st.total)});
    const float wrap = w - pad * 2;
    float h = pad + 26 * s + f.text->CalcTextSizeA(15 * s, FLT_MAX, wrap, body.c_str()).y + 16 * s;
    if (!line.empty()) h += 24 * s;
    if (fraction >= 0) h += 18 * s;
    if (!busy) h += 62 * s;
    h += pad - 10 * s;
    const ImVec2 b(a.x + w, a.y + h);
    dl->AddRectFilled(a, b, ui::WithAlpha(color::kPanel, 0.82f));
    dl->AddRectFilledMultiColor(a, b, ui::WithAlpha(color::kAccent, 0.10f), ui::WithAlpha(color::kAccent, 0.0f),
                                ui::WithAlpha(color::kAccent, 0.0f), ui::WithAlpha(color::kAccent, 0.06f));
    dl->AddRectFilled(a, ImVec2(a.x + 3 * s, b.y), color::kAccent);
    float y = a.y + pad;
    ui::DrawTracked(dl, f.display, 19 * s, ImVec2(a.x + pad, y), color::kText, heading, 2.6f * s);
    y += 34 * s;
    dl->AddText(f.text, 15 * s, ImVec2(a.x + pad, y), color::kTextDim, body.c_str(), nullptr, wrap);
    y += f.text->CalcTextSizeA(15 * s, FLT_MAX, wrap, body.c_str()).y + 14 * s;
    if (!line.empty()) {
      dl->AddText(f.text, 14 * s, ImVec2(a.x + pad, y), st.stage == Stage::kFailed ? color::kWarn : color::kText,
                  line.c_str(), nullptr, wrap);
      y += 24 * s;
    }
    if (fraction >= 0) {
      const ImVec2 r0(a.x + pad, y + 2 * s), r1(b.x - pad, y + 6 * s);
      dl->AddRectFilled(r0, r1, ui::WithAlpha(color::kText, 0.10f));
      dl->AddRectFilled(r0, ImVec2(r0.x + (r1.x - r0.x) * std::clamp(fraction, 0.0f, 1.0f), r1.y), color::kAccent);
      y += 18 * s;
    }
    if (!busy) {
      const std::string label = T("Download now");
      const ImVec2 ts = ui::TextSize(f.semibold, 15.5f * s, label);
      const ImVec2 c0(a.x + pad, y + 8 * s), c1(c0.x + ts.x + 44 * s, c0.y + 42 * s);
      const bool hot = active && ImGui::IsMouseHoveringRect(c0, c1, false);
      if (hot && in.mouse_moved()) button_focus = true;
      const bool lit = button_focus || hot;
      dl->AddRectFilled(c0, c1, lit ? color::kFocus : ui::WithAlpha(color::kText, 0.10f), 6 * s);
      dl->AddText(f.semibold, 15.5f * s, ImVec2(c0.x + 22 * s, c0.y + (42 * s - ts.y) * 0.5f),
                  lit ? color::kOnFocus : color::kText, label.c_str());
      bool press = hot && ImGui::IsMouseClicked(0);
      if (active && in.Pressed(Action::kAccept)) {
        in.Consume(Action::kAccept);
        press = true;
      }
      if (press) {
        ui::Cue(ui::Sound::kSelect);
        REXLOG_INFO("KK artbook: Download now");
        content::Service::Get().Download(content::Kind::kArtbook);
      }
    }
  }

  void Draw(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImVec2 v0, ImVec2 v1, const ui::Metrics& m, ui::Input& in,
            bool active) {
    auto& service = content::Service::Get();
    const content::Status st = service.Ready() ? service.Get(content::Kind::kArtbook) : content::Status{};
    if (!loaded && st.installed) LoadPaintings();
    TakeResults();
    if (!loaded || paintings.empty()) {
      view = -1;
      DrawMissing(dl, p0, p1, m, in, active, st);
      return;
    }
    if (view >= 0) DrawViewer(dl, v0, v1, m, in, active);
    else DrawGrid(dl, p0, p1, m, in, active);
  }

  std::vector<ui::Prompt> Prompts() const {
    if (!loaded || paintings.empty()) {
      auto& service = content::Service::Get();
      const content::Status st = service.Ready() ? service.Get(content::Kind::kArtbook) : content::Status{};
      if (st.Busy()) return {{Action::kBack, "Back"}};
      return {{Action::kAccept, "Download now"}, {Action::kBack, "Back"}};
    }
    if (view >= 0) {
      const bool zoomed = zoom_target > 1.02f;
      return {{Action::kPrevTab, "Previous"},
              {Action::kNextTab, "Next"},
              {Action::kAccept, zoomed ? "Fit to screen" : "Zoom in"},
              {Action::kResetPage, "Save to PC"},
              {Action::kBack, "Back"}};
    }
    return {{Action::kAccept, "View"}, {Action::kResetPage, "Save to PC"}, {Action::kBack, "Back"}};
  }

  void Dev(const std::string& c) {
    if (c == "state") {
      const auto st = content::Service::Get().Get(content::Kind::kArtbook);
      int thumbs = 0, fulls = 0;
      for (const Painting& p : paintings) thumbs += p.thumb.tex != nullptr, fulls += p.full.tex != nullptr;
      REXLOG_INFO("KK dev: artbook installed {} stage {} error {} paintings {} readme {} thumbs {} full {} view {} "
                  "zoom {:.3f} centre {:.0f},{:.0f} focus {} message '{}'",
                  st.installed, int(st.stage), int(st.error), paintings.size(), readme.size(), thumbs, fulls, view,
                  zoom, cx, cy, focus, message);
      for (const Painting& p : paintings)
        REXLOG_INFO("KK dev: artbook painting '{}' {}x{} thumb {} full {}", p.title, p.width, p.height,
                    int(p.thumb.state), int(p.full.state));
    } else if (c.rfind("open:", 0) == 0) {
      view_fade = 0;
      OpenViewer(std::atoi(c.c_str() + 5));
    } else if (c == "next" && view >= 0) {
      Step(1);
    } else if (c == "prev" && view >= 0) {
      Step(-1);
    } else if (c.rfind("zoom:", 0) == 0 && view >= 0) {
      ZoomTo(float(std::atof(c.c_str() + 5)), ImVec2(-1, -1));
    } else if (c.rfind("pan:", 0) == 0 && view >= 0) {
      const size_t comma = c.find_first_of(",:", 4);  // pan:DX:DY (launcher steps are split at commas)
      cx += float(std::atof(c.c_str() + 4));
      if (comma != std::string::npos) cy += float(std::atof(c.c_str() + comma + 1));
    } else if (c == "save") {
      Save(view >= 0 ? view : focus);
    } else if (c == "back") {
      view = -1;
    } else if (c == "download") {
      content::Service::Get().Download(content::Kind::kArtbook);
    }
  }
};

Artbook::Artbook(rex::ui::ImmediateDrawer* drawer, fs::path user_dir)
    : impl_(std::make_unique<Impl>(drawer, user_dir)) {}
Artbook::~Artbook() = default;

void Artbook::Draw(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImVec2 v0, ImVec2 v1, const ui::Metrics& m, ui::Input& in,
                   bool active) {
  impl_->Draw(dl, p0, p1, v0, v1, m, in, active);
}
bool Artbook::Back() {
  if (impl_->view < 0) return false;
  impl_->focus = impl_->view;
  impl_->view = -1;
  impl_->zoom = impl_->zoom_target = 1;
  return true;
}
std::vector<ui::Prompt> Artbook::Prompts() const { return impl_->Prompts(); }
void Artbook::DevCommand(const std::string& command) { impl_->Dev(command); }

}  // namespace kk
