#pragma once
#include <stdint.h>
// The graphics panel's readout (nr_effect_stats): published by the game renderer at the end of each frame.
// GPU times are the renderer's timer queries (read back later, never waited for), averaged over about 20
// frames; negative: off, or no sample yet.
typedef struct NrEffectStats {
  uint32_t size;
  uint32_t preparing;  // being made on a worker: 1 GI, 2 the lighting passes, 4 an upscaler, 8 shader variants, 16 cel shading
  double ao_ms, gi_ms, shadows_ms, water_ms, volumetric_ms, upscaler_ms;
  double tessellation_ms;
  uint32_t tessellation_draws;
  uint32_t input_width, input_height;    // the scene's size (the upscaler's input)
  uint32_t output_width, output_height;  // the render scale's (the upscaler's output)
  uint32_t upscaler_kind;                // what runs (nr::UpscalerKind; 0 off)
  uint32_t api;                          // 1 Vulkan, 2 Direct3D 12
  uint32_t vendor;                       // the GPU's PCI vendor (0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel)
  float mip_bias;                        // the upscaler's texture LOD bias for the scene's draws
  uint32_t sampler_slots, targets, retired;  // cache sizes (the robustness test watches them)
  // What the renderer applies this frame (bits): 1 ambient occlusion, 2 GI, 4 soft shadows, 8 water
  // reflections, 16 volumetric light, 32 spatial AA (FXAA / SMAA), 64 an upscaler, 128 cel shading.
  uint32_t applied;
  int32_t anisotropy;                    // the override in use (-1 the game's)
  uint32_t gi_level;                     // nr::GiLevel
  float shadow_scale;                    // the shadow maps' scale
  // Last frame's draws with the lighting options' shaders, and those still waiting for them.
  uint32_t shadow_draws, water_draws, shadow_draws_waiting, water_draws_waiting;
  uint64_t frame;
  uint64_t live_changes;                 // upscalers made again and effects put in while running
  double last_change_ms;                 // the render thread's time for the last one
  char upscaler[96];                     // "DLSS 310.9.1 Quality preset M (Vulkan)", "off"
  double cel_ms;                         // cel shading (bit 128 of applied; preparing bit 16)
  // Task 354: texture pack work not finished yet (queued and in-flight reads, an upload in progress, a folder change
  // not yet seen); 0 = the pack has streamed in everything asked for so far.
  uint32_t pack_pending;
  uint64_t pack_ready;                   // texture pack replacements finished uploading (run total)
} NrEffectStats;
#ifdef __cplusplus
namespace nr { bool ReadEffectStats(NrEffectStats* out); }
#endif
