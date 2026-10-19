// Diagnostic regression sources only. No SDK, renderer, GPU or game required.
#include "backend/puppet_mesh_probe.h"
#include "backend/draw_state.h"
#include "backend/tests/fake_guest.h"
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
void Check(bool ok, const char* what) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
class WorkerSink final : public nr::DrawSink {
 public:
  uint64_t received = 0;
  void OnClear(const nr::ClearCall&) override {}
  void OnResolve(const nr::ResolveCall&) override {}
  void OnPresent(uint32_t, uint32_t, uint32_t) override {}
  void OnDraw(const nr::DrawCall&) override {
    ++received;
    // Models the native worker reading a decoded draw after it leaves the issuer.
    // Actor scope is thread-local, so counting here would lose attribution.
    std::thread worker([&] {
      NrPuppetMeshDraws snapshot;
      nr::puppet_probe::Read(7, 0, &snapshot);
      Check(snapshot.seen == received, "counted before crossing to the render worker");
    });
    worker.join();
  }
};
}
int main() {
  using namespace nr::puppet_probe;
  Reset();
  nr::DrawCall call;
  call.frame = 42; call.vertex_buffer = 100; call.index_buffer = 200;
  call.vertex_shader = 300; call.start = 4; call.count = 600;
  NrPuppetMeshDraws a, b;
  Check(Read(1, 0, &a) && a.seen == 0, "connected zero is readable");
  const auto outer = Scope({1, 11});
  Seen(call);
  const auto nested = Scope({});
  Seen(call);  // Unrelated/ambiguous nested scope must not count against the source.
  Scope(nested);
  Seen(call);
  Scope({2, 22});
  Seen(call);  // Exactly the same buffers, shader and range, a separate actor.
  Check(Read(1, 0, &a) && Read(2, 0, &b) && a.seen == 2 && b.seen == 1,
        "source and copy sharing every draw key remain separate");
  Check(Read(1, 11, &a) && a.seen == 2 && a.last_frame == 42 && a.vertex_buffer == 100 &&
        a.index_buffer == 200 && a.vertex_shader == 300 && a.start == 4 && a.count == 600,
        "child attribution retains the decoded draw fields");
  std::thread other([&] { Seen(call); Scope({3, 33}); Seen(call); });
  other.join();
  Read(2, 0, &b);
  Check(b.seen == 1, "another thread cannot inherit this thread's actor");
  Read(3, 0, &a);
  Check(a.seen == 1, "another thread can carry its own actor");
  Scope(outer);
  Seen(call);
  Reset();
  Check(Read(1, 11, &a) && a.seen == 0 && a.vertex_buffer == 0, "unload removes counters and pointers");
  Check(!Read(1, 0, nullptr), "null snapshot is refused");
  NrPuppetDrawEvent events_out[8];
  Check(Drain(events_out, 8) == 0, "capture is off after reset");
  Capture(1, 2, 2);
  call.index_in_frame = 17; call.indexed = true; call.base_vertex = -3;
  call.vertex_info.physical = 0x1234; call.index_info.physical = 0x5678;
  call.stride = 32; call.render_targets[0] = 123;
  Scope({1, 11}); Seen(call);
  call.start = 9; call.count = 30; Seen(call);  // Preserve both ranges of one mesh.
  Scope({2, 22}); Seen(call);  // Same resource and range, different instance.
  Scope({}); Seen(call);
  Presented(42);
  Presented(43);  // A real zero-draw frame must remain visible for both roles.
  Seen(call); Presented(44);  // Capture ends after exactly two Presents.
  Check(Drain(nullptr, 8) == 0, "null event destination does not consume data");
  Check(Drain(events_out, 2) == 2 && events_out[0].owner == 1 &&
        events_out[0].start == 4 && events_out[1].start == 9,
        "partial drain retains every draw range rather than only the last draw");
  Check(events_out[0].vertex_physical == 0x1234 && events_out[0].index_physical == 0x5678 &&
        events_out[0].indexed == 1 && events_out[0].base_vertex == -3 &&
        events_out[0].ordinal == 17 && events_out[0].stride == 32 && events_out[0].target == 123,
        "capture copies buffer identity and draw/pass fields");
  Check(Drain(events_out, 8) == 3 && events_out[0].owner == 2 && events_out[0].mesh == 22 &&
        events_out[1].kind == 1 && events_out[1].frame == 42 &&
        events_out[1].source_draws == 2 && events_out[1].copy_draws == 1 &&
        events_out[1].unattributed == 1 && events_out[1].dropped == 0 &&
        events_out[2].kind == 1 && events_out[2].frame == 43 &&
        !events_out[2].source_draws && !events_out[2].copy_draws,
        "same-key actors stay separate, frame summaries include unknown and zero draws");
  Capture(1, 2, 500);  // Clamped to 120 frames, even without a consumer.
  Scope({1, 11});
  for (unsigned i = 0; i < kEventCapacity; ++i) Seen(call);
  for (unsigned i = 0; i < 121; ++i) Presented(50+i);
  unsigned records = 0, summaries = 0, lost = 0;
  while (const auto count = Drain(events_out, 8)) {
    records += count;
    for (uint32_t i = 0; i < count; ++i) if (events_out[i].kind == 1) {
      ++summaries; lost += events_out[i].dropped;
    }
  }
  Check(records == kEventCapacity && summaries == 120 && lost == 120,
        "overflow is bounded, counted and preserves all frame markers");
  Capture(1, 2, 1); Seen(call); Reset();
  Check(Drain(events_out, 8) == 0, "load reset clears pending capture identities");
  Capture(1, 1, 1); Seen(call); Presented(99);
  Check(Drain(events_out, 8) == 0, "identical actor identities refuse capture");
  Reset();
  nr::test::FakeGuestMemory memory;
  WorkerSink sink;
  nr::DrawTracker tracker(memory, &sink);
  tracker.set_find_wvp(false);
  Scope({7, 77});
  tracker.Draw(4, 0, 3);  // Missing VB: decoded draw never reaches the renderer.
  Read(7, 0, &a);
  Check(a.seen == 0 && sink.received == 0, "decode failures are not renderer-seen draws");
  uint32_t vertex_data = 0;
  tracker.SetStreamSource(0, memory.NewVertexBuffer(36, vertex_data), 0, 12);
  tracker.Draw(4, 0, 3);
  tracker.SetIndices(memory.NewIndexBuffer({0, 1, 2}, false));
  tracker.DrawIndexed(4, 0, 0, 3);
  Read(7, 0, &a);
  Check(a.seen == 2 && sink.received == 2, "plain and indexed draws both reach the probe");
  Capture(7, 8, 1);
  tracker.Draw(4, 0, 3);
  tracker.Present(4);
  Check(Drain(events_out, 8) == 2 && events_out[0].owner == 7 && events_out[1].kind == 1 &&
        events_out[1].source_draws == 1, "DrawTracker Present closes a captured frame before the worker");
  Scope({});
  Reset();
  std::puts("puppet mesh diagnostic tests passed");
}
