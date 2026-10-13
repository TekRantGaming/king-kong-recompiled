#pragma once

#include <d3d12.h>

namespace nr {

struct D3D12RequiredFormat {
  DXGI_FORMAT format;
  const char* name;
  UINT support1;
  UINT support2 = 0;
};

inline constexpr UINT kD3D12Sampled = D3D12_FORMAT_SUPPORT1_TEXTURE2D |
    D3D12_FORMAT_SUPPORT1_SHADER_LOAD | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
inline constexpr UINT kD3D12Color = kD3D12Sampled | D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
// Baseline frame, EDRAM and gamma formats. Optional AO/upscaler formats are
// checked by those passes, rather than preventing the core renderer starting.
inline constexpr D3D12RequiredFormat kD3D12RequiredFormats[] = {
    {DXGI_FORMAT_R8G8B8A8_UNORM, "RGBA8_UNORM", kD3D12Color | D3D12_FORMAT_SUPPORT1_BLENDABLE},
    {DXGI_FORMAT_R10G10B10A2_UNORM, "R10G10B10A2_UNORM", kD3D12Color | D3D12_FORMAT_SUPPORT1_BLENDABLE |
        D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW, D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE},
    {DXGI_FORMAT_R16G16B16A16_FLOAT, "RGBA16_FLOAT", kD3D12Color | D3D12_FORMAT_SUPPORT1_BLENDABLE},
    {DXGI_FORMAT_R16G16_FLOAT, "RG16_FLOAT", kD3D12Color | D3D12_FORMAT_SUPPORT1_BLENDABLE},
    {DXGI_FORMAT_R32_FLOAT, "R32_FLOAT", kD3D12Color},
    {DXGI_FORMAT_R32G32_FLOAT, "RG32_FLOAT", kD3D12Color},
    {DXGI_FORMAT_R16G16B16A16_UNORM, "RGBA16_UNORM (gamma)", kD3D12Sampled},
    {DXGI_FORMAT_D32_FLOAT_S8X24_UINT, "D32S8", D3D12_FORMAT_SUPPORT1_TEXTURE2D |
        D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL},
    {DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, "D32S8 depth SRV", kD3D12Sampled},
};

struct D3D12RequirementsResult {
  UINT binding_tier = 0;
  UINT shader_model = 0;
  UINT formats_checked = 0;
  UINT formats_supported = 0;
  bool supported = true;
};

// Query is CheckFeatureSupport, injected so failures can be tested without a
// GPU. Report receives each missing requirement, including failed queries.
template <class Query, class Report>
D3D12RequirementsResult CheckD3D12Requirements(Query query, Report report) {
  D3D12RequirementsResult result;
  auto missing = [&](const char* name) { result.supported = false; report(name); };
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  if (FAILED(query(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))) {
    missing("resource binding tier query failed");
  } else {
    result.binding_tier = UINT(options.ResourceBindingTier);
    // Tier 1 cannot expose the renderer's large, partially populated SRV and
    // sampler tables. Tier 2 suffices; no bindless CBV/UAV tables are used.
    if (options.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2)
      missing("resource binding tier 2 or higher required");
  }
  // The translated and built-in DXIL is SM 6.0. Asking for that version also
  // works on old runtimes which reject newer shader-model query enums.
  D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_0};
  if (FAILED(query(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)))) {
    missing("shader model 6.0 query failed");
  } else {
    result.shader_model = UINT(model.HighestShaderModel);
    if (model.HighestShaderModel < D3D_SHADER_MODEL_6_0) missing("shader model 6.0 required");
  }
  for (const auto& required : kD3D12RequiredFormats) {
    ++result.formats_checked;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT format{required.format};
    if (FAILED(query(D3D12_FEATURE_FORMAT_SUPPORT, &format, sizeof(format))) ||
        (UINT(format.Support1) & required.support1) != required.support1 ||
        (UINT(format.Support2) & required.support2) != required.support2) {
      missing(required.name);
    } else {
      ++result.formats_supported;
    }
  }
  return result;
}

}  // namespace nr
