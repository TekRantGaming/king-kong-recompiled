// The depth of field on the first-person hands (hands_dof.h). A plain struct, for the settings.
#pragma once

namespace nr {

struct HandsDofConfig {
  bool enabled = false;
  bool gpu_time = false;
  bool operator==(const HandsDofConfig&) const = default;
};

}  // namespace nr
