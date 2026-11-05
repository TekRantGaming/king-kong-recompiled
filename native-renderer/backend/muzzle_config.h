// Muzzle flashes that cast light (muzzle_light.h): the setting.
#pragma once

namespace nr {

struct MuzzleConfig {
  bool enabled = false;   // native_muzzle_light
  bool gpu_time = false;  // native_muzzle_gpu_time: timer queries logged
  bool operator==(const MuzzleConfig&) const = default;
};

}  // namespace nr
