#include "backend/tests/readback.h"

#include <cmath>
#include <cstring>
#include <fstream>

namespace nr::test {

Pixels ReadBackTexture(nvrhi::IDevice* device, nvrhi::ITexture* texture) {
  Pixels out;
  const nvrhi::TextureDesc& td = texture->getDesc();
  nvrhi::TextureDesc sd;
  sd.width = td.width;
  sd.height = td.height;
  sd.format = td.format;
  sd.debugName = "Readback";
  nvrhi::StagingTextureHandle staging =
      device->createStagingTexture(sd, nvrhi::CpuAccessMode::Read);
  nvrhi::CommandListHandle cl = device->createCommandList();
  cl->open();
  cl->copyTexture(staging, nvrhi::TextureSlice(), texture, nvrhi::TextureSlice());
  cl->close();
  device->executeCommandList(cl);
  device->waitForIdle();
  device->runGarbageCollection();
  size_t pitch = 0;
  auto* data = static_cast<const uint8_t*>(
      device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read,
                                       &pitch));
  if (!data) return out;
  out.width = td.width;
  out.height = td.height;
  out.rgba.resize(size_t(td.width) * td.height * 4);
  // 16-bit floats (the upscalers' motion vectors) to floats.
  auto half = [](uint32_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float f;
    if (e == 0) f = std::ldexp(float(m), -24);
    else if (e == 31) f = m ? NAN : INFINITY;
    else f = std::ldexp(float(m | 1024), int(e) - 25);
    return s ? -f : f;
  };
  const uint32_t bytes = td.format == nvrhi::Format::R8_UNORM ? 1 : 4;
  for (uint32_t y = 0; y < td.height; ++y) {
    for (uint32_t x = 0; x < td.width; ++x) {
      uint32_t v = 0;
      std::memcpy(&v, data + y * pitch + x * bytes, bytes);
      float* p = &out.rgba[(size_t(y) * td.width + x) * 4];
      if (td.format == nvrhi::Format::R8_UNORM) {
        p[0] = float(v & 255) / 255.0f;
        p[1] = p[2] = 0.0f;
        p[3] = 1.0f;
      } else if (td.format == nvrhi::Format::RG16_FLOAT) {
        p[0] = half(v & 0xFFFF);
        p[1] = half(v >> 16);
        p[2] = 0.0f;
        p[3] = 1.0f;
      } else if (td.format == nvrhi::Format::R32_FLOAT) {
        std::memcpy(&p[0], &v, 4);
        p[1] = p[2] = 0.0f;
        p[3] = 1.0f;
      } else if (td.format == nvrhi::Format::R10G10B10A2_UNORM) {
        p[0] = float(v & 1023) / 1023.0f;
        p[1] = float((v >> 10) & 1023) / 1023.0f;
        p[2] = float((v >> 20) & 1023) / 1023.0f;
        p[3] = float(v >> 30) / 3.0f;
      } else {
        for (int c = 0; c < 4; ++c) p[c] = float((v >> (8 * c)) & 255) / 255.0f;
      }
    }
  }
  device->unmapStagingTexture(staging);
  return out;
}

bool WritePpm(const std::string& path, const Pixels& pixels) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f << "P6\n" << pixels.width << " " << pixels.height << "\n255\n";
  for (size_t i = 0; i < size_t(pixels.width) * pixels.height; ++i) {
    for (int c = 0; c < 3; ++c) {
      float v = pixels.rgba[i * 4 + c];
      f.put(char(uint8_t(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f)));
    }
  }
  return bool(f);
}

}  // namespace nr::test
