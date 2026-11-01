// Task 331: the launcher's loading loops (loading_loop.h).
#include "loading_loop.h"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/immediate_drawer.h>

#include "launcher_text.h"
#include "ps2_movie_decoder.h"
#include "toast.h"

namespace kk {

namespace {
constexpr size_t kAhead = 4;  // frames decoded ahead (about 130 ms; covers reopening the file at each repeat)

struct Decoded {
  double at = 0;  // seconds on the loop's timeline
  std::vector<uint8_t> rgba;
};
}  // namespace

struct LoadingLoop::Shared {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<Decoded> queue;
  bool stop = false;
  std::atomic<bool> failed{false};
  bool once = false;       // task 332: play the file once
  double end_at = -1;      // once: the time just after its last frame, when the decoder has finished
  uint32_t width = 0, height = 0;
  float aspect = 16.0f / 9.0f;
};

namespace {
// The worker: the file again and again, each frame stamped with its time on the loop's timeline.
void Decode(std::shared_ptr<LoadingLoop::Shared> shared, std::filesystem::path file) {
  auto& s = *shared;
  double loop_start = 0;
  bool first_open = true;
  try {
    if (!movies::DecoderAvailable()) throw std::runtime_error("this build has no video decoder");
    for (;;) {
      {
        std::lock_guard lock(s.mutex);
        if (s.stop) return;
      }
      movies::VideoReader reader(file);
      if (first_open) {
        first_open = false;
        std::lock_guard lock(s.mutex);
        s.width = reader.width();
        s.height = reader.height();
        if (s.width && s.height) s.aspect = float(s.width) * reader.pixel_aspect() / float(s.height);
        REXLOG_INFO("KK loading loop: playing {} ({}x{}, {:.2f} s, {})", file.filename().string(), s.width, s.height,
                    reader.duration(), movies::DecoderName());
      }
      int64_t first = -1, last = 0, step = 333333;  // 100 ns units; 30 frames a second until measured
      int frames = 0;
      int64_t pts = 0;
      while (reader.Next(pts)) {
        if (first < 0) first = pts;
        if (frames > 0 && pts > last) step = pts - last;
        last = pts;
        ++frames;
        Decoded d;
        d.at = loop_start + double(pts - first) / 1e7;
        reader.Pixels(d.rgba);
        std::unique_lock lock(s.mutex);
        s.cv.wait(lock, [&] { return s.stop || s.queue.size() < kAhead; });
        if (s.stop) return;
        s.queue.push_back(std::move(d));
        lock.unlock();
        s.cv.notify_all();
      }
      if (frames == 0) throw std::runtime_error("no video frames");
      loop_start += double(last - first + step) / 1e7;  // seamless: the next repeat starts one frame after the last
      if (s.once) {
        std::lock_guard lock(s.mutex);
        s.end_at = loop_start;
        return;
      }
    }
  } catch (const std::exception& e) {
    REXLOG_WARN("KK loading loop: {} cannot be played ({}); not shown", file.filename().string(), e.what());
    s.failed = true;
    s.cv.notify_all();
  }
}
}  // namespace

LoadingLoop::LoadingLoop(rex::ui::ImmediateDrawer* drawer, std::filesystem::path file, bool once)
    : drawer_(drawer), shared_(std::make_shared<Shared>()) {
  shared_->once = once;
  std::error_code ec;
  if (!drawer_ || !std::filesystem::is_regular_file(file, ec)) {
    REXLOG_WARN("KK loading loop: {} is missing; not shown", file.string());
    shared_->failed = true;
    return;
  }
  worker_ = std::thread(Decode, shared_, std::move(file));
}

LoadingLoop::~LoadingLoop() {
  {
    std::lock_guard lock(shared_->mutex);
    shared_->stop = true;
  }
  shared_->cv.notify_all();
  if (worker_.joinable()) worker_.join();
  // The launcher's drawer may already be gone when the launcher closes (toast.h).
  KeepTextureAlive(std::move(texture_));
}

bool LoadingLoop::Failed() const { return shared_->failed; }

float LoadingLoop::Aspect() const {
  std::lock_guard lock(shared_->mutex);
  return shared_->aspect;
}

bool LoadingLoop::Ended() const {
  if (shared_->failed) return true;
  std::lock_guard lock(shared_->mutex);
  return shared_->end_at >= 0 && shared_->queue.empty() && position_ >= shared_->end_at;
}

bool LoadingLoop::HasFirstFrame() const {
  if (texture_) return true;
  std::lock_guard lock(shared_->mutex);
  return !shared_->queue.empty();
}

rex::ui::ImmediateTexture* LoadingLoop::Frame(float dt, double clock) {
  if (shared_->failed) return texture_.get();
  Decoded due;
  bool have = false;
  uint32_t w = 0, h = 0;
  {
    std::unique_lock lock(shared_->mutex);
    // The launcher's first frame: wait a moment for the first picture (the decoder opened the file as the launcher
    // was made), so the window never shows black first. The live menu scene's start then holds the UI thread for
    // about 1.5 s right after this frame, with the picture on screen.
    if (!texture_ && !first_waited_) {
      first_waited_ = true;
      shared_->cv.wait_for(lock, std::chrono::milliseconds(400),
                           [&] { return !shared_->queue.empty() || shared_->failed.load(); });
    }
    w = shared_->width;
    h = shared_->height;
    auto& q = shared_->queue;
    // Not before the first frame is there: the loop starts from its first frame.
    if (!q.empty() && first_at_ < 0) first_at_ = q.front().at;
    if (clock >= 0 && first_at_ >= 0) {
      // Task 332: another clock (the movie's sound) says where the picture is; late frames are dropped below.
      position_ = first_at_ + clock;
    } else {
      if (!q.empty() || texture_) position_ += std::clamp(double(dt), 0.0, 0.1);
      if (!q.empty() && !texture_) position_ = std::max(position_, q.front().at);
      // The decoder fell behind (a long frame or a busy PC): go on from its next frame instead of rushing through.
      if (!q.empty() && q.front().at < position_ - 0.25) position_ = q.front().at;
    }
    while (!q.empty() && q.front().at <= position_) {
      due = std::move(q.front());
      q.pop_front();
      have = true;
    }
  }
  if (have) {
    shared_->cv.notify_all();
    if (w && h && due.rgba.size() == size_t(w) * h * 4) {
      // The drawer frees a texture only once the frames that drew it are done (its delayed release).
      texture_ = drawer_->CreateTexture(w, h, rex::ui::ImmediateTextureFilter::kLinear, false, due.rgba.data());
    }
  }
  return texture_.get();
}

std::filesystem::path LoadingLoopFile(loading_loop::Kind kind) {
  const std::string name = kind == loading_loop::Kind::kReturning ? "returning-loop" : "save-notice-loop";
  const auto folder = rex::filesystem::GetExecutableFolder() / "launcher" / "video";
  // Their text is in the picture: <name>-<language>.mp4 beside the English one (the launcher's language).
  if (text::Current() != text::Lang::kEnglish) {
    std::error_code ec;
    auto own = folder / (name + "-" + text::kLangCodes[int(text::Current())] + ".mp4");
    if (std::filesystem::exists(own, ec)) return own;
  }
  return folder / (name + ".mp4");
}

}  // namespace kk
