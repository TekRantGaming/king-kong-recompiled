// Reading an NVRHI texture back to the CPU, for the pixel checks.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nvrhi/nvrhi.h>

namespace nr::test {

struct Pixels {
  uint32_t width = 0, height = 0;
  std::vector<float> rgba;  // width * height * 4
  const float* at(uint32_t x, uint32_t y) const { return &rgba[(size_t(y) * width + x) * 4]; }
};

// Copies a 2D R10G10B10A2_UNORM or RGBA8_UNORM texture to a staging texture,
// waits for the device and decodes it to RGBA floats.
Pixels ReadBackTexture(nvrhi::IDevice* device, nvrhi::ITexture* texture);

// Writes an image as a binary PPM (for looking at failures).
bool WritePpm(const std::string& path, const Pixels& pixels);

}  // namespace nr::test
