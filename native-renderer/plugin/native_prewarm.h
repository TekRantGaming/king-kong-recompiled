// The launcher's prewarm: exported by the native renderer plugin (rexgpu-native.dll /
// librexgpu-native.so), called by the game's launcher when it opens (native-renderer/hooks/
// kk_native_launcher.cpp), so the game's pipelines are made while the player is still in the
// launcher. docs/backend.md, "Prewarm from the launcher".
//
//   uint32_t nr_prewarm_start(const NrPrewarmRequest* request);
//   uint32_t nr_prewarm_status(NrPrewarmStatus* status);
//   void nr_prewarm_stop(void);
//
// nr_prewarm_start returns at once; the work runs on a thread of the plugin's, on a Vulkan device
// of its own, and does nothing unless --native_launcher_prewarm is on, the GPU plugin is "native"
// and the native renderer draws the game with Vulkan. The game's start (the plugin's
// SetupGuestGpu) stops it before making its own device, as nr_prewarm_stop does.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NR_PREWARM_VERSION 1

enum NrPrewarmState {
  NR_PREWARM_OFF = 0,      // not asked for, or not possible here (the log says why)
  NR_PREWARM_RUNNING = 1,  // making pipelines
  NR_PREWARM_DONE = 2,     // every description it could make is made
  NR_PREWARM_STOPPED = 3,  // stopped before it was done (the game started, or nr_prewarm_stop)
  NR_PREWARM_FAILED = 4,   // no device or renderer could be made
};

typedef struct NrPrewarmRequest {
  uint32_t size;           // sizeof(NrPrewarmRequest)
  uint32_t version;        // NR_PREWARM_VERSION
  const char* cache_root;  // UTF-8: the cache root the game will run with (<user data>/cache)
  uint32_t title_id;       // the game's title ID (0x555307D3); 0: the files are named game.*
} NrPrewarmRequest;

typedef struct NrPrewarmStatus {
  uint32_t size;      // sizeof(NrPrewarmStatus), set by the caller
  uint32_t state;     // NrPrewarmState
  uint32_t listed;    // descriptions in the pipeline files
  uint32_t queued;    // sent to the workers
  uint32_t made;      // made so far
  uint32_t waiting;   // whose shaders are not in the pack: the game makes those
  double seconds;     // since the start
} NrPrewarmStatus;

#ifdef __cplusplus
}
#endif
