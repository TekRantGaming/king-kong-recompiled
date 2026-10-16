// One progress bar for the native renderer's whole shader job (native_shaders.h): per graphics API, its
// shaders are compiled and then its pipelines prepared (two passes), each a step with its own count
// (native-renderer/plugin/native_shader_pack_api.h). The launcher shows them as ONE bar from 0 to 100%
// (owner, 16 Oct: no steps shown), each step weighted by the work it usually takes, never going backwards.
// A step that turns out to have nothing to do (already current) takes no room on the bar, so the bar does
// not jump ahead by that step's share, and the drawn bar eases towards the job's value so it moves smoothly.
#pragma once

#include <algorithm>
#include <cstdint>

namespace kk {

// What the job reports (NrShaderPackProgress, without the plugin's header).
struct ShaderJobState {
  int step = 0;       // 1-based, 0 before the first one starts
  int steps = 0;      // 2 per graphics API: its shaders, then its pipelines
  int step_kind = 0;  // 0 shaders, 1 pipelines
  int phase = 0;      // shaders: 0 starting, 1 reading, 2 compiling, 3 writing, 4 finished
  int pass = 0, passes = 2;
  uint32_t done = 0, total = 0;
  bool working = true;  // the current step is building something (not only checking that it is current)
};

// The steps' shares of the job, measured on the setup screen (task 144, RTX 4070 Ti, 15 workers, other work
// running): compiling one API's shaders took 68 to 99 seconds, preparing its pipelines 2 to 4 (longer where the
// driver has none of them cached yet), so the bar moves at an even pace.
constexpr float kShaderStepWeight = 0.45f;
constexpr float kPipelineStepWeight = 0.05f;
// Compiling ends here; writing the pack (a large file) is the rest of the shaders' step.
constexpr float kShaderCompileShare = 0.97f;
constexpr int kMaxShaderSteps = 8;

constexpr float ShaderStepWeight(int index) { return index % 2 == 0 ? kShaderStepWeight : kPipelineStepWeight; }

// How far the current step is, 0 to 1.
inline float ShaderStepFraction(const ShaderJobState& s) {
  const float counted = s.total > 0 ? std::clamp(float(s.done) / float(s.total), 0.0f, 1.0f) : 0.0f;
  if (s.step_kind == 0) {
    if (s.phase >= 4) return 1.0f;
    if (s.phase == 3) return kShaderCompileShare;
    if (s.phase == 2) return counted * kShaderCompileShare;
    return 0.0f;
  }
  const int passes = std::max(1, s.passes);
  return std::clamp((float(std::clamp(s.pass, 0, passes - 1)) + counted) / float(passes), 0.0f, 1.0f);
}

// The whole job, 0 to 1 (0 before it has steps). `skipped`: bit i set when step i+1 had nothing to do;
// those steps take no room on the bar.
inline float ShaderJobFraction(const ShaderJobState& s, uint32_t skipped = 0) {
  if (s.steps <= 0 || s.step <= 0) return 0.0f;
  const int current = std::min(s.step, s.steps) - 1;
  float sum = 0, before = 0;
  for (int i = 0; i < s.steps; ++i) {
    if (i != current && (skipped >> i) & 1u) continue;
    sum += ShaderStepWeight(i);
    if (i < current) before += ShaderStepWeight(i);
  }
  if (sum <= 0) return 0.0f;
  const float now = s.working ? ShaderStepWeight(current) * ShaderStepFraction(s) : 0.0f;
  return std::clamp((before + now) / sum, 0.0f, 1.0f);
}

// What the bar shows: the job's fraction, held where it was if a count goes back (a pass that queues
// more pipelines as it goes), with steps that had nothing to do left out.
class ShaderProgressBar {
 public:
  void Reset() {
    shown_ = drawn_ = 0;
    drawn_at_ = -1;
    skipped_ = 0;
    step_ = 0;
    step_worked_ = false;
  }
  float Update(const ShaderJobState& s) {
    if (s.step != step_) {
      // The step before this one ended: did it build anything?
      if (step_ > 0 && !step_worked_ && step_ <= kMaxShaderSteps) skipped_ |= 1u << (step_ - 1);
      step_ = s.step;
      step_worked_ = false;
    }
    step_worked_ = step_worked_ || s.working;
    shown_ = std::max(shown_, ShaderJobFraction(s, skipped_));
    return shown_;
  }
  float Shown() const { return shown_; }
  // The bar as drawn at time `now` (seconds, any clock): eases towards Shown(), never backwards, so a step's
  // count arriving in large pieces still moves the bar smoothly. Calls at the same time change nothing.
  float Drawn(double now) {
    const float seconds = drawn_at_ < 0 ? 0.0f : float(std::clamp(now - drawn_at_, 0.0, 0.25));
    drawn_at_ = now;
    const float k = std::clamp(seconds * 6.0f, 0.0f, 1.0f);
    drawn_ = std::min(shown_, std::max(drawn_, drawn_ + (shown_ - drawn_) * k));
    return drawn_;
  }
  uint32_t skipped() const { return skipped_; }

 private:
  float shown_ = 0, drawn_ = 0;
  double drawn_at_ = -1;
  uint32_t skipped_ = 0;
  int step_ = 0;
  bool step_worked_ = false;
};

}  // namespace kk
