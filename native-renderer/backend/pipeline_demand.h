// Task 179: a frame never waits for a scene draw's missing pipeline (DECISIONS.md, 17 Oct, "Shader
// stutter work"). The pipeline is made on the workers and the draw is skipped until it is ready (a
// frame or two of pop-in instead of a freeze); draws that later draws depend on still wait.
//
// The interface is the one a cheaper creation path (Vulkan graphics pipeline libraries) plugs into:
// a caller requests a pipeline with a PipelineDemand; the answer is the pipeline when it is ready, or
// null (the caller's fallback: skip the draw / keep the original pass) while a worker makes it.
#pragma once

#include <cstdint>
#include <iterator>
#include <utility>
#include "backend/pipeline_cache.h"

namespace nr {

// How a caller asks for a pipeline (GameRenderer::GetPipeline).
enum class PipelineDemand : uint8_t {
  kNow,       // the renderer's own passes: made at once on the calling thread when absent (a small fixed set)
  kMustDraw,  // a game draw later draws depend on: queued first; the draw waits for it within a cap per frame
              // (task 267: Options::pipeline_must_wait_ms), then is skipped like a kMaySkip draw
  kMaySkip,   // a game scene draw: queued after other draws' jobs; null until a worker has made it
  kOptional,  // an extra pass (motion replay, lighting variant, memory blit): queued behind, never counted
  kSoon,      // an extra pass the scene shows (AO on the first-person hands): queued with the draws' jobs
              // (ahead of the prewarm), never waited for, never counted
};

// Why a game draw's missing pipeline is waited for, or kScene when the draw may be skipped.
enum class PipelineMissClass : uint8_t {
  kScene,        // depth-tested draw that writes colour: opaque geometry, decals, transparent, particles, water
  kDepthOnly,    // writes no colour: depth prepass, shadow map, stencil-only mask, occlusion draw
  kStencil,      // writes stencil: later draws test it (a skipped mask would light or hide the wrong pixels)
  kScreenSpace,  // no depth test (or ALWAYS): post-processing, fades, composites, copies, HUD
  kLoading,      // the game is loading: its last frame may stay on screen (the V-Rex loading screen)
  kBlockAll,     // pipeline_wait (KK_DEV_PIPELINE_BLOCK=1): every draw waits, the old behaviour
  kCount
};

inline const char* PipelineMissClassName(PipelineMissClass c) {
  switch (c) {
    case PipelineMissClass::kScene: return "scene";
    case PipelineMissClass::kDepthOnly: return "depth-only";
    case PipelineMissClass::kStencil: return "stencil";
    case PipelineMissClass::kScreenSpace: return "screen-space";
    case PipelineMissClass::kLoading: return "loading";
    case PipelineMissClass::kBlockAll: return "block-all";
    case PipelineMissClass::kCount: break;
  }
  return "?";
}

// From the draw's pipeline record (the register values its state comes from).
inline PipelineMissClass ClassifyPipelineMiss(const PipelineRecord& r) {
  bool writes_color = false;
  for (uint32_t i = 0; i < r.color_count && i < 4; ++i) writes_color = writes_color || (r.color_mask[i] & 0xF) != 0;
  if (!writes_color) return PipelineMissClass::kDepthOnly;
  const bool depth = r.depth_format != 0;
  const uint32_t dc = r.depth_control;
  if (depth && (dc & 1) && ((r.ref_mask >> 16) & 0xFF) != 0) {
    // RB_DEPTHCONTROL stencil ops (fail, pass, depth fail; 0 = KEEP), front and (bit 7) back faces.
    bool writes = ((dc >> 11) & 7) || ((dc >> 14) & 7) || ((dc >> 17) & 7);
    if (dc & 0x80) writes = writes || ((dc >> 23) & 7) || ((dc >> 26) & 7) || ((dc >> 29) & 7);
    if (writes) return PipelineMissClass::kStencil;
  }
  if (!depth || !(dc & 2) || ((dc >> 4) & 7) == 7) return PipelineMissClass::kScreenSpace;
  return PipelineMissClass::kScene;
}

// The demand for a game draw whose pipeline is not made yet.
inline PipelineDemand DrawDemand(PipelineMissClass c) {
  return c == PipelineMissClass::kScene ? PipelineDemand::kMaySkip : PipelineDemand::kMustDraw;
}

// The workers' queue (jobs with .key and .urgent): a waited-for draw's job goes first; a skipped
// draw's after the draws' jobs already queued (first use first) and before the prewarm's.
template <class Queue, class Job>
void QueueDrawJob(Queue& jobs, Job job, bool first) {
  job.urgent = true;
  if (first) {
    jobs.push_front(std::move(job));
    return;
  }
  auto at = jobs.begin();
  while (at != jobs.end() && at->urgent) ++at;
  jobs.insert(at, std::move(job));
}

// A queued (not started) job a draw now asks for moves up the same way. False: not queued (a worker
// has it already, or it is done).
template <class Queue>
bool PromoteDrawJob(Queue& jobs, uint64_t key, bool first) {
  auto queued = jobs.begin();
  while (queued != jobs.end() && queued->key != key) ++queued;
  if (queued == jobs.end()) return false;
  if (queued->urgent && !first) return true;  // already among the draws' jobs
  auto job = std::move(*queued);
  jobs.erase(queued);
  QueueDrawJob(jobs, std::move(job), first);
  return true;
}

// Task 267: how long a draw later draws depend on (kMustDraw) may still wait for its pipeline, given what
// this frame's such draws have waited already. cap_ms < 0: no limit (the pre-267 behaviour, and
// block-all); 0: no wait. After the cap the draw is skipped like a scene draw (pop-in, not a freeze).
inline double MustDrawWaitLeft(double cap_ms, double waited_this_frame_ms) {
  if (cap_ms < 0) return -1.0;
  const double left = cap_ms - waited_this_frame_ms;
  return left > 0 ? left : 0.0;
}

}  // namespace nr
