// NVIDIA DLSS Super Resolution and DLAA (upscaler.h). Built when NR_DLSS_SDK_DIR names the DLSS SDK.

#include "backend/upscaler.h"

namespace nr {

#if 1  // until the DLSS stage
std::unique_ptr<Upscaler> CreateDlssUpscaler(nvrhi::IDevice*, const UpscaleConfig&, std::string* why) {
  if (why) *why = "not built in (the DLSS SDK was not found at build time: NR_DLSS_SDK_DIR)";
  return nullptr;
}
#endif

}  // namespace nr
