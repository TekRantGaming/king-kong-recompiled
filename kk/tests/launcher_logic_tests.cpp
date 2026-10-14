// Console checks of launcher logic that needs no game, window or device: the shader job's single
// progress bar (shader_progress.h).
#include "shader_progress.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
int checks = 0;
void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}
bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

void ShaderProgressChecks() {
  using kk::ShaderJobFraction;
  using kk::ShaderJobState;
  ShaderJobState s;
  Check(ShaderJobFraction(s) == 0.0f, "no steps yet: 0");
  // Two APIs: four steps, shaders 42% each and pipelines 8% each.
  s.steps = 4;
  s.step = 1;
  s.phase = 1;  // reading the database
  Check(ShaderJobFraction(s) == 0.0f, "step 1 reading: 0");
  s.phase = 2;
  s.total = 1000;
  s.done = 500;
  Check(Near(ShaderJobFraction(s), 0.42f * 0.5f * kk::kShaderCompileShare), "step 1 half compiled");
  s.phase = 3;
  Check(Near(ShaderJobFraction(s), 0.42f * kk::kShaderCompileShare), "step 1 writing");
  s.phase = 4;
  Check(Near(ShaderJobFraction(s), 0.42f), "step 1 finished");
  s.step = 2;
  s.step_kind = 1;
  s.done = s.total = 0;
  s.pass = 0;
  Check(Near(ShaderJobFraction(s), 0.42f), "step 2 starts where step 1 ended");
  s.total = 100;
  s.done = 50;
  Check(Near(ShaderJobFraction(s), 0.42f + 0.08f * 0.25f), "step 2 first pass half");
  s.pass = 1;
  s.done = 100;
  Check(Near(ShaderJobFraction(s), 0.50f), "step 2 both passes done");
  s.step = 3;
  s.step_kind = 0;
  s.phase = 0;
  s.done = s.total = 0;
  Check(Near(ShaderJobFraction(s), 0.50f), "step 3 starts at half");
  s.step = 4;
  s.step_kind = 1;
  s.pass = 1;
  s.total = s.done = 10;
  Check(Near(ShaderJobFraction(s), 1.0f), "the last step done: 100%");
  // One API (the other failed to start on this PC): two steps over the whole bar.
  ShaderJobState one;
  one.steps = 2;
  one.step = 2;
  one.step_kind = 1;
  Check(Near(ShaderJobFraction(one), 0.42f / 0.5f), "one API: its shaders are 84%");
  // Out-of-range reports stay on the bar.
  ShaderJobState odd;
  odd.steps = 2;
  odd.step = 7;
  odd.step_kind = 1;
  odd.pass = 5;
  odd.done = 50;
  odd.total = 10;
  Check(ShaderJobFraction(odd) <= 1.0f && ShaderJobFraction(odd) >= 0.0f, "out-of-range counts are clamped");

  // A whole job, sampled as the launcher polls it: the bar never goes back and ends at 100%.
  kk::ShaderProgressBar bar;
  float last = 0;
  bool monotonic = true;
  auto poll = [&](const ShaderJobState& st) {
    const float f = bar.Update(st);
    monotonic = monotonic && f >= last;
    last = f;
  };
  for (int api = 0; api < 2; ++api) {
    ShaderJobState st;
    st.steps = 4;
    st.step = api * 2 + 1;
    for (int phase = 0; phase <= 1; ++phase) { st.phase = phase; poll(st); }
    st.phase = 2;
    st.total = 17790;
    for (uint32_t d = 0; d <= st.total; d += 997) { st.done = d; poll(st); }
    st.phase = 3; poll(st);
    st.phase = 4; poll(st);
    st.step = api * 2 + 2;
    st.step_kind = 1;
    st.phase = 4;
    for (int pass = 0; pass < 2; ++pass) {
      st.pass = pass;
      st.done = st.total = 0;
      poll(st);
      // The pass queues more pipelines as it goes: the count goes back, the bar must not.
      st.total = 3000;
      st.done = 2000;
      poll(st);
      st.total = 3700;
      st.done = 2100;
      poll(st);
      st.done = 3700;
      poll(st);
    }
  }
  Check(monotonic, "the bar never goes back over a whole job");
  Check(Near(last, 1.0f), "a whole job ends at 100%");
  bar.Reset();
  Check(bar.Shown() == 0.0f, "a new job starts at 0");
}
}  // namespace

int main() {
  ShaderProgressChecks();
  std::cout << checks << " launcher checks passed\n";
  return 0;
}
