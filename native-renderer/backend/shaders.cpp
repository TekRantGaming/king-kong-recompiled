#include "backend/shaders.h"

#include <cstdint>

#include "backend/shaders/generated/placeholder_ps_dxil.h"
#include "backend/shaders/generated/placeholder_ps_spirv.h"
#include "backend/shaders/generated/placeholder_vs_dxil.h"
#include "backend/shaders/generated/placeholder_vs_spirv.h"
#include "backend/shaders/generated/triangle_ps_dxil.h"
#include "backend/shaders/generated/triangle_ps_spirv.h"
#include "backend/shaders/generated/triangle_vs_dxil.h"
#include "backend/shaders/generated/triangle_vs_spirv.h"

namespace nr {

ShaderBytecode GetShaderBytecode(nvrhi::GraphicsAPI api, ShaderId id) {
  const bool spirv = api == nvrhi::GraphicsAPI::VULKAN;
#define NR_BLOB(name) \
  (spirv ? ShaderBytecode{k_##name##_spirv, sizeof(k_##name##_spirv)} \
         : ShaderBytecode{k_##name##_dxil, sizeof(k_##name##_dxil)})
  switch (id) {
    case ShaderId::kTriangleVs: return NR_BLOB(triangle_vs);
    case ShaderId::kTrianglePs: return NR_BLOB(triangle_ps);
    case ShaderId::kPlaceholderVs: return NR_BLOB(placeholder_vs);
    case ShaderId::kPlaceholderPs: return NR_BLOB(placeholder_ps);
  }
#undef NR_BLOB
  return {};
}

}  // namespace nr
