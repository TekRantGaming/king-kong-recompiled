// nr_trace_replay <log>: replays a KK_DEV_D3D_TRACE log through the hook
// table and the draw-state tracker and prints, per frame, what would reach
// the renderer. The log has no guest memory, so draws are counted as dropped
// (their buffers cannot be read); the call routing, the per-frame counts and
// the state the engine sets are what this checks against a real trace.

#include <cstdio>
#include <fstream>
#include <map>

#include "backend/api_binding.h"
#include "backend/draw_state.h"
#include "backend/tests/fake_guest.h"
#include "hooks/trace_replay.h"

namespace {

class CountingSink final : public nr::DrawSink {
 public:
  void OnClear(const nr::ClearCall&) override { ++clears; }
  void OnDraw(const nr::DrawCall&) override { ++draws; }
  void OnResolve(const nr::ResolveCall&) override { ++resolves; }
  void OnPresent(uint32_t frame, uint32_t rt0, uint32_t) override {
    std::printf("frame %u: %u clears, %u resolves, render target 0 at present %08X\n", frame,
                clears, resolves, rt0);
    clears = draws = resolves = 0;
  }
  uint32_t clears = 0, draws = 0, resolves = 0;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: nr_trace_replay <KK_DEV_D3D_TRACE log>\n");
    return 2;
  }
  std::ifstream log(argv[1]);
  if (!log) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }
  nr::test::FakeGuestMemory memory(16, 16);  // nothing mapped
  CountingSink sink;
  nr::DrawTracker state(memory, &sink);
  nr::ApiBinding binding;
  nr::InitApiBinding(binding, &state, [](void* s) { return static_cast<nr::DrawTracker*>(s); });
  nr::hooks::ReplayStats stats = nr::hooks::ReplayTrace(log, &binding.api);
  const auto& s = state.stats();
  std::printf(
      "%llu lines, %llu calls: %llu to hooked entry points, %llu to others\n"
      "%llu draws (%llu indexed), %llu clears, %llu resolves, %llu presents\n",
      (unsigned long long)stats.lines, (unsigned long long)stats.calls,
      (unsigned long long)stats.hooked, (unsigned long long)stats.unhooked,
      (unsigned long long)s.draws, (unsigned long long)s.indexed_draws,
      (unsigned long long)s.clears, (unsigned long long)s.resolves,
      (unsigned long long)s.presents);
  return 0;
}
