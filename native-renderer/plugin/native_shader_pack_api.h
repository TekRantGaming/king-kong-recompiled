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

#define NR_SHADER_PACK_API_VERSION 1

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
} NrShaderPackProgress;

typedef struct NrShaderPackApi {
  uint32_t version;  // NR_SHADER_PACK_API_VERSION
  uint32_t size;     // sizeof(NrShaderPackApi)

  // Starts making sure the pack the native renderer would use with the current settings (the
  // plugin's cvars: native_backend, native_render_scale, native_shadow_scale, native_dxc) is in
  // <cache_dir>/native-shaders and current, building it from <game_dir>/Shaders/xeshaders.bin
  // if not, on a thread of its own. threads 0: one per hardware thread. low_priority: the
  // workers run below normal priority (the launcher stays smooth). Never null.
  NrShaderPackJob* (*start)(const char* cache_dir, const char* game_dir, uint32_t threads, int low_priority);
  void (*poll)(NrShaderPackJob* job, NrShaderPackProgress* out);
  // Asks the build to stop soon; nothing is written.
  void (*cancel)(NrShaderPackJob* job);
  // Waits for the job's thread (cancel first to make that quick) and frees the job.
  void (*release)(NrShaderPackJob* job);
} NrShaderPackApi;

#ifdef __cplusplus
}
#endif
