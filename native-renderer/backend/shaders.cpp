#include "backend/shaders.h"

#include <cstdint>
#include "backend/shaders/generated/memory_depth_ps_dxil.h"
#include "backend/shaders/generated/memory_depth_ps_spirv.h"

#include "backend/shaders/generated/blit_ps_dxil.h"
#include "backend/shaders/generated/blit_ps_spirv.h"
#include "backend/shaders/generated/blit_vs_dxil.h"
#include "backend/shaders/generated/blit_vs_spirv.h"
#include "backend/shaders/generated/clear_ps_dxil.h"
#include "backend/shaders/generated/clear_ps_spirv.h"
#include "backend/shaders/generated/clear_vs_dxil.h"
#include "backend/shaders/generated/clear_vs_spirv.h"
#include "backend/shaders/generated/placeholder_ps_dxil.h"
#include "backend/shaders/generated/placeholder_ps_spirv.h"
#include "backend/shaders/generated/placeholder_vs_dxil.h"
#include "backend/shaders/generated/placeholder_vs_spirv.h"
#include "backend/shaders/generated/triangle_ps_dxil.h"
#include "backend/shaders/generated/triangle_ps_spirv.h"
#include "backend/shaders/generated/triangle_vs_dxil.h"
#include "backend/shaders/generated/triangle_vs_spirv.h"
#include "backend/shaders/generated/ao_vs_dxil.h"
#include "backend/shaders/generated/ao_vs_spirv.h"
#include "backend/shaders/generated/ao_copy_ps_dxil.h"
#include "backend/shaders/generated/ao_copy_ps_spirv.h"
#include "backend/shaders/generated/ao_depth_ps_dxil.h"
#include "backend/shaders/generated/ao_depth_ps_spirv.h"
#include "backend/shaders/generated/ao_ao_ps_dxil.h"
#include "backend/shaders/generated/ao_ao_ps_spirv.h"
#include "backend/shaders/generated/ao_blur_ps_dxil.h"
#include "backend/shaders/generated/ao_blur_ps_spirv.h"
#include "backend/shaders/generated/ao_upsample_ps_dxil.h"
#include "backend/shaders/generated/ao_upsample_ps_spirv.h"
#include "backend/shaders/generated/ao_apply_ps_dxil.h"
#include "backend/shaders/generated/ao_apply_ps_spirv.h"

#include "backend/shaders/generated/smaa_edge_vs_dxil.h"
#include "backend/shaders/generated/smaa_edge_vs_spirv.h"
#include "backend/shaders/generated/smaa_edge_ps_dxil.h"
#include "backend/shaders/generated/smaa_edge_ps_spirv.h"
#include "backend/shaders/generated/smaa_weight_vs_dxil.h"
#include "backend/shaders/generated/smaa_weight_vs_spirv.h"
#include "backend/shaders/generated/smaa_weight_ps_dxil.h"
#include "backend/shaders/generated/smaa_weight_ps_spirv.h"
#include "backend/shaders/generated/smaa_blend_vs_dxil.h"
#include "backend/shaders/generated/smaa_blend_vs_spirv.h"
#include "backend/shaders/generated/smaa_blend_ps_dxil.h"
#include "backend/shaders/generated/smaa_blend_ps_spirv.h"

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
    case ShaderId::kBlitVs: return NR_BLOB(blit_vs);
    case ShaderId::kBlitPs: return NR_BLOB(blit_ps);
    case ShaderId::kClearVs: return NR_BLOB(clear_vs);
    case ShaderId::kClearPs: return NR_BLOB(clear_ps);
    case ShaderId::kMemoryDepthPs: return NR_BLOB(memory_depth_ps);
    case ShaderId::kAoVs: return NR_BLOB(ao_vs);
    case ShaderId::kAoCopyPs: return NR_BLOB(ao_copy_ps);
    case ShaderId::kAoDepthPs: return NR_BLOB(ao_depth_ps);
    case ShaderId::kAoPs: return NR_BLOB(ao_ao_ps);
    case ShaderId::kAoBlurPs: return NR_BLOB(ao_blur_ps);
    case ShaderId::kAoUpsamplePs: return NR_BLOB(ao_upsample_ps);
    case ShaderId::kAoApplyPs: return NR_BLOB(ao_apply_ps);
    case ShaderId::kSmaaEdgeVs: return NR_BLOB(smaa_edge_vs);
    case ShaderId::kSmaaEdgePs: return NR_BLOB(smaa_edge_ps);
    case ShaderId::kSmaaWeightVs: return NR_BLOB(smaa_weight_vs);
    case ShaderId::kSmaaWeightPs: return NR_BLOB(smaa_weight_ps);
    case ShaderId::kSmaaBlendVs: return NR_BLOB(smaa_blend_vs);
    case ShaderId::kSmaaBlendPs: return NR_BLOB(smaa_blend_ps);
  }
#undef NR_BLOB
  return {};
}

}  // namespace nr
