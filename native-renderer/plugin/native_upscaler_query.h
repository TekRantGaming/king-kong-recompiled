// Which temporal upscalers this PC can run, for the launcher's Display > Upscaling menu: exported by
// the native renderer plugin (rexgpu-native.dll), read by native-renderer/hooks/kk_native_ui.cpp.
// docs/upscalers.md, "The launcher's menu".
//
//   uint32_t nr_upscaler_query(NrUpscalerSupport* out);
//   void nr_upscaler_query_wait(void);
//
// The first nr_upscaler_query starts the question on a thread of the plugin's and returns at once
// (NR_UPSCALER_QUERY_RUNNING); later calls return the state, and once it is NR_UPSCALER_QUERY_DONE
// fill `out`. The answer comes from QueryUpscalerSupport (backend/upscaler.h) on a Direct3D 12 device
// of the query's own on the same graphics card: each SDK says whether it runs on this card and driver.
// The SDK answer is cached; each call filters it for the current native_backend so changing the API
// in the launcher immediately changes the choices. With Vulkan, FSR 4 is left out (AMD's FSR 4 has
// no Vulkan); DLSS and XeSS run on
// Vulkan through the SDK's device requests (sdk-patches/0003), else they are left out too.
// nr_upscaler_query_wait returns once the thread is done (the game's start and the launcher's restart
// call it before they make devices of their own).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum NrUpscalerQueryState {
  NR_UPSCALER_QUERY_OFF = 0,      // not possible here (no Direct3D 12, no temporal upscalers built in)
  NR_UPSCALER_QUERY_RUNNING = 1,  // asking the SDKs
  NR_UPSCALER_QUERY_DONE = 2,     // `out` has the answer
  NR_UPSCALER_QUERY_FAILED = 3,   // no device could be made (the log says why)
};

typedef struct NrUpscalerSupport {
  uint32_t size;     // sizeof(NrUpscalerSupport), set by the caller
  uint32_t state;    // NrUpscalerQueryState
  uint32_t vendor;   // PCI vendor of the graphics card: 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel
  int32_t vulkan;    // 1: the answer is for Vulkan (the game's backend), 0: Direct3D 12
  int32_t fsr3, fsr4, dlss, xess;  // 1: offered
  char notes[1024];  // why the others are not, for the log
} NrUpscalerSupport;

#ifdef __cplusplus
}
#endif
