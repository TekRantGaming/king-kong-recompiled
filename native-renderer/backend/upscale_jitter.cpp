#include "backend/upscale_jitter.h"

#include <algorithm>
#include <cmath>

namespace nr {

float Halton(uint32_t index, uint32_t base) {
  float f = 1.0f, r = 0.0f;
  while (index > 0) {
    f /= float(base);
    r += f * float(index % base);
    index /= base;
  }
  return r;
}

uint32_t JitterPhaseCount(float ratio_x, float ratio_y) {
  const double n = std::ceil(8.0 * double(std::max(ratio_x, 1.0f)) * double(std::max(ratio_y, 1.0f)) - 1e-4);
  return uint32_t(std::max(8.0, n));
}

Jitter JitterForFrame(uint64_t frame, uint32_t phases) {
  if (phases == 0) phases = 8;
  const uint32_t i = uint32_t(frame % phases) + 1;
  return {Halton(i, 2) - 0.5f, Halton(i, 3) - 0.5f};
}

void JitterNdcOffset(const Jitter& j, uint32_t width, uint32_t height, bool vulkan, float& ox, float& oy) {
  // NDC spans 2 units over the target: one host pixel is 2 / size. D3D's NDC y points up, so moving
  // the picture down is a negative offset; on Vulkan the shaders negate y after it.
  ox = 2.0f * j.x / float(std::max(width, 1u));
  oy = -2.0f * j.y / float(std::max(height, 1u));
  if (vulkan) oy = -oy;
}

float UpscaleMipBias(float ratio) {
  if (!(ratio > 0.0f)) return 0.0f;
  return -std::log2(ratio);
}

}  // namespace nr
