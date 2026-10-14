// The C interface the launcher (king_kong.exe) uses to build the native renderer's shader pack on
// the player's machine, from their own copy of the game (docs/shaders.md, "The pack on the
// player's machine"). The builder lives in the plugin (rexgpu-native.dll / librexgpu-native.so),
// which has the translator and loads DXC; the launcher finds it in the plugin it already loaded
// for its settings (PreloadGpuPlugin).
//
// Exported by the plugin: const NrShaderPackApi* nr_get_shader_pack_api(void). The pointer is
// valid for the life of the process. Paths are UTF-8.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NR_SHADER_PACK_API_VERSION 3  // 2: start takes force; 3: the job's steps

typedef struct NrShaderPackJob NrShaderPackJob;

typedef struct NrShaderPackProgress {
  int phase;          // 0 starting (DXC, database), 1 reading, 2 compiling, 3 writing, 4 finished
  uint32_t done;      // shaders compiled so far
  uint32_t total;     // shaders to compile (0 until the database is read)
  uint32_t failed;
  int finished;       // 1 once the build is over (the fields below are set)
  int ok;             // the pack is there and current
  int up_to_date;     // it already was: nothing was built
  int cancelled;
  double seconds;
  char backend[8];    // "spirv" or "dxil"
  char pack[1024];    // the pack's path
  char message[512];  // what happened, for the log (an error, or a summary)
  // The job's steps, for one progress bar over all of it: per graphics API, its shaders
  // (step_kind 0: done/total count shaders) and then its pipelines (step_kind 1: done/total count
  // pipelines, in `passes` passes, `pass` the current one). `backend` is the step's API. A step with
  // nothing to do ends at once.
  int step;           // 1-based, 0 before the first one starts
  int steps;          // how many the job has (2 per graphics API)
  int step_kind;      // 0 shaders, 1 pipelines
  int pass, passes;   // pipelines: the current pass (0-based) and how many there are
  int working;        // 1 while something is being compiled or prepared (not only checked)
} NrShaderPackProgress;

typedef struct NrShaderPackApi {
  uint32_t version;  // NR_SHADER_PACK_API_VERSION
  uint32_t size;     // sizeof(NrShaderPackApi)

  // Ensures both API packs, including scale, water and motion variants, are current in
  // <cache_dir>/native-shaders, building them from <game_dir>/Shaders/xeshaders.bin
  // if not, on a thread of its own. threads 0: one per hardware thread. low_priority: the
  // workers run below normal priority (the launcher stays smooth). force: build it even when it is
  // current (the launcher's "recompile shaders"). Listed pipelines are prepared on headless
  // devices before finishing, with a readiness stamp so Play checks do not repeat preparation.
  // Never null.
  NrShaderPackJob* (*start)(const char* cache_dir, const char* game_dir, uint32_t threads, int low_priority,
                            int force);
  void (*poll)(NrShaderPackJob* job, NrShaderPackProgress* out);
  // Asks the build to stop soon; nothing is written.
  void (*cancel)(NrShaderPackJob* job);
  // Waits for the job's thread (cancel first to make that quick) and frees the job.
  void (*release)(NrShaderPackJob* job);
} NrShaderPackApi;

#ifdef __cplusplus
}
#endif
