#pragma once

#include <array>
#include <cstdint>
#include <nvrhi/nvrhi.h>

namespace nr {

// Bindless descriptor tables do not make NVRHI track their textures. Upload
// every fallback and explicitly make it readable before any table is used.
// The gamma texture is bound even when the blit does not apply a gamma ramp.
template <class CommandList, class Texture, class Buffer>
void UploadFallbackResources(CommandList* cl, Texture* tex2d, Texture* tex3d,
                             Texture* cube, Texture* array, Buffer* buffer, Texture* gamma) {
  const uint32_t black = 0;
  for (Texture* texture : {tex2d, tex3d, array})
    cl->writeTexture(texture, 0, 0, &black, sizeof(black));
  for (uint32_t face = 0; face < 6; ++face)
    cl->writeTexture(cube, face, 0, &black, sizeof(black));
  const std::array<uint32_t, 64> zeros{};
  cl->writeBuffer(buffer, zeros.data(), sizeof(zeros));
  std::array<uint16_t, 1024> ramp;
  for (uint32_t i = 0; i < 256; ++i) {
    ramp[i * 4] = ramp[i * 4 + 1] = ramp[i * 4 + 2] = uint16_t(i * 257);
    ramp[i * 4 + 3] = 0xFFFF;
  }
  cl->writeTexture(gamma, 0, 0, ramp.data(), sizeof(ramp));
  for (Texture* texture : {tex2d, tex3d, cube, array, gamma})
    cl->setTextureState(texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
  cl->setBufferState(buffer, nvrhi::ResourceStates::ShaderResource);
  cl->commitBarriers();
}

}  // namespace nr
