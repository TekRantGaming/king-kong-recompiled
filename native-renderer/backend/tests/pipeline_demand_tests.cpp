// Task 179: which draws may be skipped while their pipeline is made (ClassifyPipelineMiss) and the
// workers' queue order (draws' jobs first use first, a waited-for draw's at the front, prewarm behind).
#include "backend/pipeline_demand.h"
#include <cstdio>
#include <cstdlib>
#include <deque>

static int cases = 0;
static void Check(bool condition, const char* what) {
  ++cases;
  if (!condition) {
    std::fprintf(stderr, "case %d failed: %s\n", cases, what);
    std::exit(1);
  }
}

int main() {
  using namespace nr;
  constexpr uint32_t kD24S8 = 1;  // any non-zero format
  PipelineRecord scene;
  scene.color_count = 1;
  scene.color_mask[0] = 0xF;
  scene.depth_format = kD24S8;
  scene.depth_control = 0x2 | 0x4 | (3u << 4);  // test + write, LEQUAL
  Check(ClassifyPipelineMiss(scene) == PipelineMissClass::kScene, "opaque geometry may skip");
  PipelineRecord r = scene;
  r.depth_control = 0x2 | (3u << 4);  // test, no write: transparent, particles, decals
  r.color_control[0] = 0x00050004;    // blended
  Check(ClassifyPipelineMiss(r) == PipelineMissClass::kScene, "transparent may skip");
  r = scene;
  r.color_mask[0] = 0;
  Check(ClassifyPipelineMiss(r) == PipelineMissClass::kDepthOnly, "depth prepass waits");
  r = scene;
  r.color_count = 0;
  Check(ClassifyPipelineMiss(r) == PipelineMissClass::kDepthOnly, "no colour attachment waits");
  r = scene;
  r.depth_control |= 1u | (2u << 14);  // stencil on, REPLACE on pass
  r.ref_mask = 0xFFu << 16;
  Check(ClassifyPipelineMiss(r) == PipelineMissClass::kStencil, "stencil write waits");
  r.ref_mask = 0;  // write mask 0: only tests the stencil
  Check(ClassifyPipelineMiss(r) == PipelineMissClass::kScene, "stencil test only may skip");
  r = scene;
  r.depth_control |= 1u | 0x80u | (6u << 29);  // back face INCR only
  r.ref_mask = 0xFFu << 16;
  Check(ClassifyPipelineMiss(r) == PipelineMissClass::kStencil, "back-face stencil write waits");
  r = scene;
  r.depth_format = 0;
  Check(ClassifyPipelineMiss(r) == PipelineMissClass::kScreenSpace, "no depth target waits");
  r = scene;
  r.depth_control = 0;
  Check(ClassifyPipelineMiss(r) == PipelineMissClass::kScreenSpace, "depth test off waits (fade, post)");
  r = scene;
  r.depth_control = 0x2 | (7u << 4);
  Check(ClassifyPipelineMiss(r) == PipelineMissClass::kScreenSpace, "depth ALWAYS waits");
  Check(DrawDemand(PipelineMissClass::kScene) == PipelineDemand::kMaySkip, "scene demand");
  Check(DrawDemand(PipelineMissClass::kLoading) == PipelineDemand::kMustDraw, "loading demand");
  Check(DrawDemand(PipelineMissClass::kBlockAll) == PipelineDemand::kMustDraw, "block-all demand");

  struct Job {
    uint64_t key = 0;
    bool urgent = false;
  };
  std::deque<Job> q = {{100, false}, {101, false}, {102, false}};  // prewarm
  QueueDrawJob(q, Job{1}, false);
  QueueDrawJob(q, Job{2}, false);
  Check(q[0].key == 1 && q[1].key == 2 && q[2].key == 100, "skipped draws' jobs first use first, before prewarm");
  QueueDrawJob(q, Job{3}, true);
  Check(q[0].key == 3 && q[1].key == 1, "a waited-for draw's job at the front");
  Check(PromoteDrawJob(q, 101, false) && q[3].key == 101 && q[3].urgent && q[4].key == 100,
        "a queued prewarm job a draw asks for joins the draws' jobs");
  Check(PromoteDrawJob(q, 2, true) && q[0].key == 2, "a draw that must wait moves its job to the front");
  Check(PromoteDrawJob(q, 1, false) && q[2].key == 1, "already a draw's job: stays");
  Check(!PromoteDrawJob(q, 999, false), "not queued");
  Check(q.size() == 6, "nothing duplicated or lost");
  std::printf("nr_pipeline_demand_tests: %d cases passed\n", cases);
  return 0;
}
