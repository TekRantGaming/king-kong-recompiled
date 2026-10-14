#pragma once
#include <stdint.h>
// Existing nonblocking GPU timers, sampled after Present. Negative means no completed sample.
typedef struct NrEffectStats {
  uint32_t size;
  uint32_t preparing;
  double ao_ms, gi_ms, shadows_ms, water_ms, volumetric_ms;
} NrEffectStats;
#ifdef __cplusplus
namespace nr { bool ReadEffectStats(NrEffectStats* out); }
#endif
