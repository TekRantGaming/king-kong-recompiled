// --native_ao_gpu_time (docs/ao.md, "Cost"): the GPU time of the ambient occlusion's passes,
// with NVRHI timer queries around each piece of AO work (the legacy passes at the scene copy;
// GTAO's compute passes, its depth copy and its apply passes). Results are read back a few
// frames later, never waited for, and logged as an average every 600 frames with AO work.
// Render thread only.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <deque>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "backend/log.h"

namespace nr {

class AoGpuTimer {
 public:
  // label: the log line's start (the upscalers time their work with it too).
  AoGpuTimer(nvrhi::IDevice* device, const char* what, const char* label = "AO GPU time")
      : device_(device), what_(what), label_(label) {}
  ~AoGpuTimer() { if (trace_) std::fclose(trace_); }
  void EnableTrace(const char* suffix) {
    if (const char* path = std::getenv("KK_DEV_DLSS_LOG"); path && *path) {
      trace_ = std::fopen((std::string(path) + suffix).c_str(), "w");
      if (trace_) std::fprintf(trace_, "frame,gpu_ms\n");
    }
  }

  // Around one piece of AO work in a command list (Begin and End in the same list).
  void Begin(nvrhi::ICommandList* cl) {
    if (open_) return;
    nvrhi::TimerQueryHandle q;
    if (!free_.empty()) {
      q = free_.back();
      free_.pop_back();
    } else if (pending_.size() < 256) {
      q = device_->createTimerQuery();
    }
    if (!q) return;
    cl->beginTimerQuery(q);
    open_ = q;
  }
  void End(nvrhi::ICommandList* cl) {
    if (!open_) return;
    cl->endTimerQuery(open_);
    pending_.push_back({open_, frame_});
    open_ = nullptr;
  }

  // Once a frame (at Present): reads the finished queries; logs every 600 frames that had AO work.
  void FrameEnd() {
    while (!pending_.empty() && device_->pollTimerQuery(pending_.front().query)) {
      Pending p = pending_.front();
      pending_.pop_front();
      const double seconds = double(device_->getTimerQueryTime(p.query));
      total_s_ += seconds;
      if (trace_) std::fprintf(trace_, "%llu,%.6f\n", static_cast<unsigned long long>(p.frame), 1000 * seconds);
      device_->resetTimerQuery(p.query);
      free_.push_back(p.query);
      if (p.frame != last_frame_counted_) {
        last_frame_counted_ = p.frame;
        ++frames_;
      }
      ++pieces_;
    }
    ++frame_;
    if (trace_ && frame_ % 300 == 0) std::fflush(trace_);
    if (frames_ >= 600) {
      Logf(LogLevel::kInfo, "rexgpu-native: %s (%s): %.3f ms a frame on average over %llu frames (%llu timed passes)",
           label_, what_, 1000.0 * total_s_ / double(frames_), static_cast<unsigned long long>(frames_),
           static_cast<unsigned long long>(pieces_));
      total_s_ = 0.0;
      frames_ = pieces_ = 0;
    }
  }

 private:
  struct Pending {
    nvrhi::TimerQueryHandle query;
    uint64_t frame = 0;
  };
  nvrhi::IDevice* device_;
  FILE* trace_ = nullptr;
  const char* what_;
  const char* label_;
  nvrhi::TimerQueryHandle open_;
  std::deque<Pending> pending_;
  std::vector<nvrhi::TimerQueryHandle> free_;
  uint64_t frame_ = 0;
  uint64_t last_frame_counted_ = UINT64_MAX;
  double total_s_ = 0.0;
  uint64_t frames_ = 0, pieces_ = 0;
};

}  // namespace nr
