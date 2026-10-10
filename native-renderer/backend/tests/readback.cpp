#include "backend/tests/readback.h"

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
  for (uint32_t y = 0; y < td.height; ++y) {
    for (uint32_t x = 0; x < td.width; ++x) {
      uint32_t v;
      std::memcpy(&v, data + y * pitch + x * 4, 4);
      float* p = &out.rgba[(size_t(y) * td.width + x) * 4];
      if (td.format == nvrhi::Format::R10G10B10A2_UNORM) {
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
