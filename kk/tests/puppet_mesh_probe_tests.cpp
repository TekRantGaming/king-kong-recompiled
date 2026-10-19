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
  Scope({});
  Reset();
  std::puts("puppet mesh diagnostic tests passed");
}
