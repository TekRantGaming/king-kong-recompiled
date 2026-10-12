// The pipeline stutter counter for the game's overlay: exported by the native renderer plugin,
// read by native-renderer/hooks/kk_native_ui.cpp. docs/backend.md, "Stutter counter".
//
//   uint32_t nr_stutter_stats(NrStutterStats* stats);
//
// 1 with the counters as they were at the game's last Present, when --native_stutter_stats is on
// and the game renderer draws; 0 (and nothing written) otherwise.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NrStutterStats {
  uint32_t size;              // sizeof(NrStutterStats), set by the caller
  uint32_t reserved;
  uint64_t frames;            // the game's frames so far
  uint64_t stutter_frames;    // frames in which a draw waited for its pipeline or was skipped
  uint64_t frames_waited;     // frames in which a draw waited for its pipeline
  uint64_t frames_skipped;    // frames in which a draw was skipped, its pipeline not ready
  uint64_t draws_skipped;     // those draws
  uint64_t worst_wait_frame;  // the frame of the worst wait
  double wait_ms;             // all the waiting
  double worst_wait_ms;       // the most one frame waited for pipelines
} NrStutterStats;

#ifdef __cplusplus
}
#endif
