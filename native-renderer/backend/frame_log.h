// REX_DEV_FRAME_LOG for the native plugin: the lines the Xenos plugin writes for the
// frames it is asked to log (docs/testing.md, "The frame log format"; patches 0003 and
// 0012 in tools/rexglue-patches), so tests/compare.py can diff the draw list of a
// native run against a golden Xenos one.
//
//   REX_DEV_FRAME_LOG=<seconds>[,<frames>]
//
// Clock and frames, as the Xenos plugin has them: the clock starts at the first draw,
// resolve or swap of the run; a swap at least <seconds> after that starts logging
// ("frame start"); the next swap ends the frame ("frame end, <d> draws, <r> resolves"),
// and so on for <frames> frames (default 1), after which the log stays quiet.
//
// This file has no renderer in it: the caller fills one record per draw and resolve,
// and the lines go to an emit function (the plugin's log at warning level). The clock is
// injectable for tests.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nr {

struct FrameLogConfig {
  bool enabled = false;
  double start_after = 0.0;  // seconds on the log's clock
  int frames = 1;
  // "<seconds>[,<frames>]" as the environment variable has it; null or empty: disabled. Like
  // the Xenos plugin: atof / atoi, frames at least 1.
  static FrameLogConfig Parse(const char* text);
};

struct FrameLogTarget {
  uint32_t index = 0, base = 0, format = 0;
};

struct FrameLogDraw {
  uint64_t ps_hash = 0, vs_hash = 0;
  uint32_t pitch = 0, msaa = 1;
  bool z_test = false, z_write = false;
  uint32_t z_func = 0;
  uint32_t depth_base = 0, depth_format = 0;
  int32_t offset_x = 0, offset_y = 0;
  uint32_t scissor_w = 0, scissor_h = 0;
  std::vector<FrameLogTarget> targets;
  uint32_t count = 0;
};

struct FrameLogResolve {
  bool from_depth = false;
  uint32_t source_index = 0, source_base = 0, source_format = 0;
  uint32_t dest_address = 0, dest_format = 0, dest_pitch = 0, dest_height = 0;
  uint32_t command = 0;  // xenos::CopyCommand: 0 raw, 1 convert, 2 constant one, 3 null
  bool clear_color = false, clear_depth = false;
  int32_t offset_x = 0, offset_y = 0;
  uint32_t surface_pitch = 0, msaa = 1;
};

class FrameLog {
 public:
  using Emit = std::function<void(const std::string& line)>;
  using Clock = std::function<double()>;  // seconds, monotonic

  // clock: null = std::chrono::steady_clock.
  FrameLog(const FrameLogConfig& config, Emit emit, Clock clock = nullptr);

  bool enabled() const { return config_.enabled; }
  // Logging a frame right now: draws and resolves should be reported.
  bool active() const { return active_; }

  // A swap (the game's Present). Ends the frame being logged, or starts one.
  void OnSwap();
  // Only while active(); each takes the next index of the frame.
  void Draw(const FrameLogDraw& draw);
  // After Draw, for the draw just logged: its pipeline was not ready (placeholder) or could not
  // be made (missing), so the draw was not drawn.
  void DrawNotDrawn(bool pending);
  void Resolve(const FrameLogResolve& resolve);
  // The clock starts at the first call of any of the above; this starts it without logging.
  void Touch();

  static std::string FormatDraw(uint32_t index, const FrameLogDraw& draw);
  static std::string FormatResolve(uint32_t index, const FrameLogResolve& resolve);

  uint32_t draws() const { return draws_; }
  uint32_t resolves() const { return resolves_; }

 private:
  FrameLogConfig config_;
  Emit emit_;
  Clock clock_;
  bool started_ = false;
  double start_ = 0.0;
  bool active_ = false;
  int frames_left_ = 0;
  uint32_t draws_ = 0, resolves_ = 0;
};

}  // namespace nr
