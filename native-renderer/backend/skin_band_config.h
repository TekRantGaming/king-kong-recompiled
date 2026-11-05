// The red band on human skin (skin_band.h). A plain struct, for the settings.
#pragma once

namespace nr {

struct SkinBandConfig {
  bool enabled = false;
  bool gpu_time = false;
  bool operator==(const SkinBandConfig&) const = default;
};

}  // namespace nr
