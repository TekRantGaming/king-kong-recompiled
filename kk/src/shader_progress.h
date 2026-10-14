// One progress bar for the native renderer's whole shader job (native_shaders.h): per graphics
// API, its shaders are compiled and then its pipelines prepared (two passes), each a step with its
// own count (native-renderer/plugin/native_shader_pack_api.h). The launcher shows them as one bar
// from 0 to 100%, each step weighted by the work it usually takes, and never going backwards.
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
};

// The steps' shares of the job, measured on the setup screen: compiling one API's shaders takes about
// 60 to 80 seconds, preparing its pipelines a few (more on a first run without a driver cache).
constexpr float kShaderStepWeight = 0.42f;
constexpr float kPipelineStepWeight = 0.08f;
// Compiling ends here; writing the pack (a large file) is the rest of the shaders' step.
constexpr float kShaderCompileShare = 0.97f;

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

// The whole job, 0 to 1 (0 before it has steps).
inline float ShaderJobFraction(const ShaderJobState& s) {
  if (s.steps <= 0 || s.step <= 0) return 0.0f;
  const int current = std::min(s.step, s.steps) - 1;
  float sum = 0, before = 0;
  for (int i = 0; i < s.steps; ++i) {
    sum += ShaderStepWeight(i);
    if (i < current) before += ShaderStepWeight(i);
  }
  return std::clamp((before + ShaderStepWeight(current) * ShaderStepFraction(s)) / sum, 0.0f, 1.0f);
}

// What the bar shows: the job's fraction, held where it was if a count goes back (a pass that queues
// more pipelines as it goes).
class ShaderProgressBar {
 public:
  void Reset() { shown_ = 0; }
  float Update(const ShaderJobState& s) {
    shown_ = std::max(shown_, ShaderJobFraction(s));
    return shown_;
  }
  float Shown() const { return shown_; }

 private:
  float shown_ = 0;
};

}  // namespace kk
