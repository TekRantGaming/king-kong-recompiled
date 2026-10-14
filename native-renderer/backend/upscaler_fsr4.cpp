// AMD FSR 4 (FSR Upscaling 4, machine learning) through AMD's FSR SDK 2.3 (upscaler.h, docs/upscalers.md).
//
// The SDK's signed DLLs, loaded at run time from the game's folder: amd_fidelityfx_loader_dx12.dll (the
// FidelityFX API) and amd_fidelityfx_upscaler_dx12.dll (the upscalers it offers: FSR 4.1.1 on AMD Radeon
// RX 9000 and RX 7000 GPUs, FSR 3.1.5 everywhere). The SDK has no Vulkan support, so FSR 4 is D3D12
// only. The upscaler asks the library which versions this device can run and takes the newest FSR 4;
// when there is none (NVIDIA, Intel, older AMD GPUs, Vulkan) it is not available and the choice falls
// back to FSR 3.1 (upscaler_ffx.cpp).
//
// The SDK 2.3 headers (MIT, thirdparty/FSR-SDK-API) differ from the FidelityFX SDK 1.1.4 ones FSR 3.1
// uses, so they are kept in a namespace of their own here.

#include "backend/upscaler.h"

#if NR_HAS_FFX && NR_HAS_D3D12

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include <stdbool.h>
#include <stdint.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <dxgi1_5.h>

#include "backend/log.h"

namespace nr_fsr_sdk2 {
#include "api/include/dx12/ffx_api_dx12.h"
#include "api/include/ffx_api.h"
#include "api/include/ffx_api_types.h"
#include "upscalers/include/ffx_upscale.h"
}  // namespace nr_fsr_sdk2

namespace nr {

namespace {

namespace f = nr_fsr_sdk2;

struct Fsr4Functions {
  HMODULE dll = nullptr;
  f::PfnFfxCreateContext CreateContext = nullptr;
  f::PfnFfxDestroyContext DestroyContext = nullptr;
  f::PfnFfxQuery Query = nullptr;
  f::PfnFfxDispatch Dispatch = nullptr;
  bool ok() const { return CreateContext && DestroyContext && Query && Dispatch; }
};

const Fsr4Functions* Library() {
  static const Fsr4Functions fns = [] {
    Fsr4Functions fn;
    const std::wstring path = UpscalerDllFolder() + L"amd_fidelityfx_loader_dx12.dll";
    fn.dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!fn.dll) return fn;
    fn.CreateContext = reinterpret_cast<f::PfnFfxCreateContext>(GetProcAddress(fn.dll, "ffxCreateContext"));
    fn.DestroyContext = reinterpret_cast<f::PfnFfxDestroyContext>(GetProcAddress(fn.dll, "ffxDestroyContext"));
    fn.Query = reinterpret_cast<f::PfnFfxQuery>(GetProcAddress(fn.dll, "ffxQuery"));
    fn.Dispatch = reinterpret_cast<f::PfnFfxDispatch>(GetProcAddress(fn.dll, "ffxDispatch"));
    return fn;
  }();
  return fns.dll ? &fns : nullptr;
}

uint32_t Quality(UpscaleQuality q) {
  switch (q) {
    case UpscaleQuality::kNative: return f::FFX_UPSCALE_QUALITY_MODE_NATIVEAA;
    case UpscaleQuality::kQuality: return f::FFX_UPSCALE_QUALITY_MODE_QUALITY;
    case UpscaleQuality::kBalanced: return f::FFX_UPSCALE_QUALITY_MODE_BALANCED;
    case UpscaleQuality::kPerformance: return f::FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;
    case UpscaleQuality::kUltraPerformance: return f::FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE;
  }
  return f::FFX_UPSCALE_QUALITY_MODE_QUALITY;
}

void Message(uint32_t type, const wchar_t* message) {
  static int count = 0;
  if (++count > 50 || !message) return;
  std::string text;
  for (const wchar_t* p = message; *p; ++p) text += *p < 128 ? char(*p) : '?';
  Logf(type == f::FFX_API_MESSAGE_TYPE_ERROR ? LogLevel::kWarning : LogLevel::kInfo, "rexgpu-native: FSR 4: %s",
       text.c_str());
}

class Fsr4Upscaler final : public Upscaler {
 public:
  Fsr4Upscaler(nvrhi::IDevice* device, const UpscaleConfig& config, const Fsr4Functions* fn, uint64_t version_id,
               std::string version_name)
      : device_(device), config_(config), fn_(fn), version_id_(version_id), version_name_(std::move(version_name)) {}
  ~Fsr4Upscaler() override { Destroy(); }

  std::string Name() const override { return "FSR " + version_name_ + " (D3D12)"; }

  UpscalerSize InputSize(UpscalerSize output) const override {
    // FSR's documented ratios (1, 1.5, 1.7, 2, 3), the same for FSR 4.
    const float ratio = DefaultUpscaleRatio(config_.quality);
    return {UpscaleInputSize(output.width, ratio), UpscaleInputSize(output.height, ratio)};
  }

  bool Evaluate(nvrhi::ICommandList* cl, const UpscalerInputs& in) override {
    if (!in.color || !in.depth || !in.motion || !in.output) return false;
    const nvrhi::TextureDesc& cd = in.color->getDesc();
    const nvrhi::TextureDesc& od = in.output->getDesc();
    if (!context_ || cd.width != max_render_.width || cd.height != max_render_.height || od.width != max_output_.width ||
        od.height != max_output_.height) {
      if (!Create({cd.width, cd.height}, {od.width, od.height})) return false;
    }
    cl->clearState();
    cl->setTextureState(in.color, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    cl->setTextureState(in.depth, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    cl->setTextureState(in.motion, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    if (in.reactive) cl->setTextureState(in.reactive, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    cl->setTextureState(in.output, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    cl->commitBarriers();

    auto res = [](nvrhi::ITexture* t, uint32_t state) {
      return f::ffxApiGetResourceDX12(static_cast<ID3D12Resource*>(t->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource).pointer),
                                      state);
    };
    f::ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cl->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList).pointer;
    d.color = res(in.color, f::FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.depth = res(in.depth, f::FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.motionVectors = res(in.motion, f::FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    if (in.reactive) d.reactive = res(in.reactive, f::FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.output = res(in.output, f::FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    // The same conventions as FSR 3.1 (upscaler_ffx.cpp).
    d.jitterOffset = {in.jitter_x, in.jitter_y};
    d.motionVectorScale = {1.0f, 1.0f};
    d.renderSize = {in.render.width, in.render.height};
    d.upscaleSize = {in.output_size.width, in.output_size.height};
    d.enableSharpening = in.sharpness >= 0.0f;
    d.sharpness = in.sharpness >= 0.0f ? in.sharpness : 0.0f;
    const auto now = std::chrono::steady_clock::now();
    float dt = in.frame_ms;
    if (last_.time_since_epoch().count()) dt = std::chrono::duration<float, std::milli>(now - last_).count();
    last_ = now;
    d.frameTimeDelta = dt > 0.0f && dt < 200.0f ? dt : 33.3f;
    d.preExposure = 1.0f;
    d.reset = in.reset;
    d.cameraNear = in.camera_near;
    d.cameraFar = in.camera_far;
    d.cameraFovAngleVertical = in.camera_fov_y;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = f::FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    const f::ffxReturnCode_t rc = fn_->Dispatch(&context_, &d.header);
    cl->clearState();
    if (rc != f::FFX_API_RETURN_OK) {
      if (failures_++ < 10) Logf(LogLevel::kWarning, "rexgpu-native: FSR 4 dispatch failed (%u)", rc);
      return false;
    }
    return true;
  }

 private:
  void Destroy() {
    if (!context_) return;
    UpscalerWaitForIdle(device_);
    fn_->DestroyContext(&context_, nullptr);
    context_ = nullptr;
  }

  bool Create(UpscalerSize render, UpscalerSize output) {
    Destroy();
    f::ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.flags = f::FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    desc.maxRenderSize = {render.width, render.height};
    desc.maxUpscaleSize = {output.width, output.height};
    desc.fpMessage = Message;
    f::ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = static_cast<ID3D12Device*>(device_->getNativeObject(nvrhi::ObjectTypes::D3D12_Device).pointer);
    f::ffxCreateContextDescUpscaleVersion api{};
    api.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    api.version = FFX_UPSCALER_VERSION;
    f::ffxOverrideVersion override_version{};
    override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    override_version.versionId = version_id_;
    desc.header.pNext = &backend.header;
    backend.header.pNext = &api.header;
    api.header.pNext = &override_version.header;
    const f::ffxReturnCode_t rc = fn_->CreateContext(&context_, &desc.header, nullptr);
    if (rc != f::FFX_API_RETURN_OK || !context_) {
      Logf(LogLevel::kWarning, "rexgpu-native: FSR 4 context not created (%u) for %ux%u to %ux%u", rc, render.width,
           render.height, output.width, output.height);
      context_ = nullptr;
      return false;
    }
    max_render_ = render;
    max_output_ = output;
    f::ffxQueryGetProviderVersion v{};
    v.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (fn_->Query(&context_, &v.header) == f::FFX_API_RETURN_OK && v.versionName) version_name_ = v.versionName;
    Logf(LogLevel::kInfo, "rexgpu-native: %s context for %ux%u to %ux%u", Name().c_str(), render.width, render.height,
         output.width, output.height);
    return true;
  }

  nvrhi::IDevice* device_;
  UpscaleConfig config_;
  const Fsr4Functions* fn_;
  uint64_t version_id_;
  std::string version_name_;
  f::ffxContext context_ = nullptr;
  UpscalerSize max_render_, max_output_;
  std::chrono::steady_clock::time_point last_{};
  uint32_t failures_ = 0;
};

}  // namespace

std::unique_ptr<Upscaler> CreateFsr4Upscaler(nvrhi::IDevice* device, const UpscaleConfig& config, std::string* why) {
  if (device->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D12) {
    if (why) *why = "FSR 4 needs Direct3D 12 (AMD's FSR SDK has no Vulkan version)";
    return nullptr;
  }
  const Fsr4Functions* fn = Library();
  if (!fn) {
    if (why) *why = "amd_fidelityfx_loader_dx12.dll is not beside the game";
    return nullptr;
  }
  if (!fn->ok()) {
    if (why) *why = "amd_fidelityfx_loader_dx12.dll has no FidelityFX API";
    return nullptr;
  }
  // The upscaler versions the library offers on this device (newest first), and the newest FSR 4.
  f::ffxQueryDescGetVersions q{};
  q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  q.device = device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device).pointer;
  uint64_t count = 0;
  q.outputCount = &count;
  if (fn->Query(nullptr, &q.header) != f::FFX_API_RETURN_OK || count == 0) {
    if (why) *why = "the AMD upscaler library offers no upscaler here (amd_fidelityfx_upscaler_dx12.dll missing?)";
    return nullptr;
  }
  std::vector<uint64_t> ids(count);
  std::vector<const char*> names(count);
  q.versionIds = ids.data();
  q.versionNames = names.data();
  if (fn->Query(nullptr, &q.header) != f::FFX_API_RETURN_OK) {
    if (why) *why = "the AMD upscaler library did not list its versions";
    return nullptr;
  }
  std::string offered;
  for (uint64_t i = 0; i < count; ++i) {
    // (The names may live in memory a later query overwrites: copied now.)
    const std::string name = names[i] ? names[i] : "?";
    offered += (offered.empty() ? "" : ", ") + name;
    if (!name.empty() && name[0] == '4') {
      return std::make_unique<Fsr4Upscaler>(device, config, fn, ids[i], name);
    }
  }
  if (why) *why = "FSR 4 does not run on this GPU (the AMD library offers FSR " + offered + ")";
  return nullptr;
}

}  // namespace nr

#else

namespace nr {
std::unique_ptr<Upscaler> CreateFsr4Upscaler(nvrhi::IDevice* device, const UpscaleConfig&, std::string* why) {
  if (why) {
    *why = device && device->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D12
               ? "FSR 4 needs Direct3D 12 (AMD's FSR SDK has no Vulkan version)"
               : "not built in";
  }
  return nullptr;
}
}  // namespace nr

#endif
