// AMD FSR 3.1 and FSR 4 through the FidelityFX API (upscaler.h).

#include "backend/upscaler.h"

namespace nr {

#if 1  // until the FSR stage
std::unique_ptr<Upscaler> CreateFsr3Upscaler(nvrhi::IDevice*, const UpscaleConfig&, std::string* why) {
  if (why) *why = "not built in";
  return nullptr;
}
std::unique_ptr<Upscaler> CreateFsr4Upscaler(nvrhi::IDevice*, const UpscaleConfig&, std::string* why) {
  if (why) *why = "not built in";
  return nullptr;
}
#endif

}  // namespace nr
