#include <iterator>
#include <string>
#include <vector>

#include "backend/d3d12_requirements.h"
#include "backend/tests/check.h"

namespace {
struct Features {
  UINT tier = 2;
  D3D_SHADER_MODEL model = D3D_SHADER_MODEL_6_0;
  int failed_feature = -1;
  DXGI_FORMAT bad_format = DXGI_FORMAT_UNKNOWN;
  UINT missing1 = 0, missing2 = 0;
  std::vector<std::string> errors;

  nr::D3D12RequirementsResult Check() {
    return nr::CheckD3D12Requirements([&](D3D12_FEATURE feature, void* data, UINT) -> HRESULT {
      if (int(feature) == failed_feature) return E_FAIL;
      if (feature == D3D12_FEATURE_D3D12_OPTIONS)
        static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS*>(data)->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER(tier);
      if (feature == D3D12_FEATURE_SHADER_MODEL) {
        auto* m = static_cast<D3D12_FEATURE_DATA_SHADER_MODEL*>(data);
        CHECK(m->HighestShaderModel == D3D_SHADER_MODEL_6_0);
        m->HighestShaderModel = model;
      }
      if (feature == D3D12_FEATURE_FORMAT_SUPPORT) {
        auto* f = static_cast<D3D12_FEATURE_DATA_FORMAT_SUPPORT*>(data);
        f->Support1 = D3D12_FORMAT_SUPPORT1(~(f->Format == bad_format ? missing1 : 0u));
        f->Support2 = D3D12_FORMAT_SUPPORT2(~(f->Format == bad_format ? missing2 : 0u));
      }
      return S_OK;
    }, [&](const char* error) { errors.emplace_back(error); });
  }
};
}

TEST(D3D12Requirements_Tier2And3AcceptSM60) {
  for (UINT tier : {2u, 3u}) {
    Features f;
    f.tier = tier;
    auto r = f.Check();
    CHECK(r.supported);
    CHECK(f.errors.empty());
    CHECK_EQ(r.binding_tier, tier);
    CHECK_EQ(r.shader_model, UINT(D3D_SHADER_MODEL_6_0));
    CHECK_EQ(r.formats_supported, std::size(nr::kD3D12RequiredFormats));
  }
}

TEST(D3D12Requirements_RejectTier1AndOldShaders) {
  Features f;
  f.tier = 1;
  f.model = D3D_SHADER_MODEL_5_1;
  CHECK(!f.Check().supported);
  CHECK_EQ(f.errors.size(), 2u);
}

TEST(D3D12Requirements_QueryFailuresAreFatalAndReportAll) {
  for (auto feature : {D3D12_FEATURE_D3D12_OPTIONS, D3D12_FEATURE_SHADER_MODEL, D3D12_FEATURE_FORMAT_SUPPORT}) {
    Features f;
    f.failed_feature = int(feature);
    auto r = f.Check();
    CHECK(!r.supported);
    CHECK_EQ(r.formats_checked, std::size(nr::kD3D12RequiredFormats));
    CHECK_EQ(f.errors.size(), feature == D3D12_FEATURE_FORMAT_SUPPORT ? std::size(nr::kD3D12RequiredFormats) : 1u);
  }
}

TEST(D3D12Requirements_EachRequiredFormatBitIsChecked) {
  for (auto required : nr::kD3D12RequiredFormats) {
    for (UINT bit = 1; bit; bit <<= 1) {
      for (bool support2 : {false, true}) {
        if (!((support2 ? required.support2 : required.support1) & bit)) continue;
        Features f;
        f.bad_format = required.format;
        (support2 ? f.missing2 : f.missing1) = bit;
        auto r = f.Check();
        CHECK(!r.supported);
        CHECK_EQ(r.formats_supported + 1, r.formats_checked);
        CHECK_EQ(f.errors.size(), 1u);
        CHECK_EQ(f.errors[0], std::string(required.name));
      }
    }
  }
}

NR_TEST_MAIN()
