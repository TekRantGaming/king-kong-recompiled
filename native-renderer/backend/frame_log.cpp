#include "backend/frame_log.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nr {

namespace {

std::string Format(const char* format, ...)
#if defined(__clang__) || defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

std::string Format(const char* format, ...) {
  char buffer[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  return buffer;
}

}  // namespace

FrameLogConfig FrameLogConfig::Parse(const char* text) {
  FrameLogConfig config;
  if (!text || !*text) return config;
  config.enabled = true;
  config.start_after = std::atof(text);
  if (const char* comma = std::strchr(text, ',')) config.frames = std::max(1, std::atoi(comma + 1));
  // The Xenos plugin treats a negative start as off.
  if (config.start_after < 0.0) config.enabled = false;
  return config;
}

FrameLog::FrameLog(const FrameLogConfig& config, Emit emit, Clock clock)
    : config_(config), emit_(std::move(emit)), clock_(std::move(clock)), frames_left_(config.frames) {
  if (!clock_) {
    clock_ = [] {
      return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
  }
}

void FrameLog::Touch() {
  if (!started_) {
    started_ = true;
    start_ = clock_();
  }
}

void FrameLog::OnSwap() {
  Touch();
  if (!config_.enabled) return;
  if (active_) {
    emit_(Format("Frame log: frame end, %u draws, %u resolves", draws_, resolves_));
    draws_ = resolves_ = 0;
    if (--frames_left_ <= 0) {
      active_ = false;
      config_.enabled = false;
    }
    return;
  }
  if (clock_() - start_ >= config_.start_after) {
    active_ = true;
    draws_ = resolves_ = 0;
    emit_("Frame log: frame start");
  }
}

void FrameLog::Draw(const FrameLogDraw& draw) {
  Touch();
  if (!active_) return;
  emit_(FormatDraw(draws_++, draw));
}

void FrameLog::DrawNotDrawn(bool pending) {
  if (!active_ || draws_ == 0) return;
  emit_(Format("Frame log: draw %u pipeline %s", draws_ - 1, pending ? "placeholder (skipped)" : "missing"));
}

void FrameLog::Resolve(const FrameLogResolve& resolve) {
  Touch();
  if (!active_) return;
  emit_(FormatResolve(resolves_++, resolve));
}

std::string FrameLog::FormatDraw(uint32_t index, const FrameLogDraw& d) {
  std::string targets;
  for (const FrameLogTarget& t : d.targets) targets += Format(" rt%u@%u:f%u", t.index, t.base, t.format);
  return Format(
      "Frame log: draw %u ps %016llX vs %016llX pitch %u msaa %u z %s%s func %u @%u:f%u off %d,%d scissor %ux%u%s count %u",
      index, static_cast<unsigned long long>(d.ps_hash), static_cast<unsigned long long>(d.vs_hash), d.pitch, d.msaa,
      d.z_test ? "test" : "off", d.z_write ? "+write" : "", d.z_func, d.depth_base, d.depth_format, d.offset_x,
      d.offset_y, d.scissor_w, d.scissor_h, targets.c_str(), d.count);
}

std::string FrameLog::FormatResolve(uint32_t index, const FrameLogResolve& r) {
  const std::string source = r.from_depth ? Format("depth@%u:f%u", r.source_base, r.source_format)
                                          : Format("rt%u@%u:f%u", r.source_index, r.source_base, r.source_format);
  return Format(
      "Frame log: resolve %u %s -> %08X format %u pitch %u height %u command %u clear color %u depth %u off %d,%d "
      "surface pitch %u msaa %u",
      index, source.c_str(), r.dest_address, r.dest_format, r.dest_pitch, r.dest_height, r.command,
      r.clear_color ? 1u : 0u, r.clear_depth ? 1u : 0u, r.offset_x, r.offset_y, r.surface_pitch, r.msaa);
}

}  // namespace nr
