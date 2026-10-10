// The backend's compiled shaders (generated/*.h, from compile_shaders.sh):
// DXIL for D3D12, SPIR-V for Vulkan.
#pragma once

#include <cstddef>

#include <nvrhi/nvrhi.h>

namespace nr {

struct ShaderBytecode {
  const void* data = nullptr;
  size_t size = 0;
};

enum class ShaderId {
  kTriangleVs,
  kTrianglePs,
  kPlaceholderVs,
  kPlaceholderPs,
  kBlitVs,
  kBlitPs,
  kClearVs,
  kClearPs,
};

// Empty bytecode when the API has no build of the shader.
ShaderBytecode GetShaderBytecode(nvrhi::GraphicsAPI api, ShaderId id);

}  // namespace nr
