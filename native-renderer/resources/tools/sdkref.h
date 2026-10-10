// Shared between kknr_sdkref's CPU emulation (sdkref.cpp) and its GPU run of the SDK's load shaders
// (sdkref_gpu.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kknr_sdkref {

// One host subresource as the SDK's texture cache fills it: tightly packed host blocks.
struct RefSubresource {
  uint32_t level = 0, layer = 0;
  uint32_t width_blocks = 0, height_blocks = 0, depth = 0;
  uint32_t bytes_per_block = 0;
  std::vector<uint8_t> blocks;
};

// Runs the SDK's own D3D12 texture load shaders (the bytecode the game's renderer uses) on the GPU with the
// dispatches D3D12TextureCache::LoadTextureDataFromResidentMemoryImpl makes, and reads back every host
// subresource. memory is the guest physical memory the fetch constant's addresses point into.
bool SdkGpuReference(const uint32_t words[6], const std::vector<uint8_t>& memory, std::vector<RefSubresource>& out,
                     std::string& why);

}  // namespace kknr_sdkref
