#pragma once

#include "backend/ao_config.h"
#include "backend/gi_config.h"
#include "backend/lighting_config.h"

namespace nr {
// Polled once at the start of a render frame. Files and upscaler settings stay at startup.
struct EffectSettings {
  AoQuality ao_quality = AoQuality::kFull;
  GiConfig gi;
  LightingConfig lighting;
  float shadow_scale = 0;
};
}  // namespace nr
