// Intel XeSS Super Resolution (upscaler.h). Built when NR_XESS_SDK_DIR names the XeSS SDK.

#include "backend/upscaler.h"

namespace nr {

#if 1  // until the XeSS stage
std::unique_ptr<Upscaler> CreateXessUpscaler(nvrhi::IDevice*, const UpscaleConfig&, std::string* why) {
  if (why) *why = "not built in (the XeSS SDK was not found at build time: NR_XESS_SDK_DIR)";
  return nullptr;
}
#endif

}  // namespace nr
