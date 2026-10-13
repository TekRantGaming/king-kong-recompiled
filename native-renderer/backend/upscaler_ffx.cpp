// AMD FSR 3.1 through the FidelityFX API (upscaler.h, docs/upscalers.md).
//
// The API's signed DLLs from the FidelityFX SDK 1.1.4 (FSR 3.1.4), loaded at run time from the game's
// folder: amd_fidelityfx_vk.dll on Vulkan, amd_fidelityfx_dx12.dll on D3D12. FSR 3.1 runs on every GPU
// with either API. The headers are vendored (MIT, thirdparty/FidelityFX-API). FSR 4 (D3D12, the FSR
// SDK 2.x) is upscaler_fsr4.cpp.
//
// The game renderer's NVRHI command list is open while FSR records into its native command buffer:
// the inputs are put in shader-read state and the output in unordered-access state through NVRHI
// first, FSR leaves them as it found them, and NVRHI's cached state is cleared afterwards (FSR binds
// its own pipelines, descriptor sets and, on D3D12, descriptor heaps).

#include "backend/upscaler.h"

#if NR_HAS_FFX

#include <chrono>
#include <cstring>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "backend/log.h"
#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_api_loader.h"
#include "ffx_api/ffx_upscale.h"
#if NR_HAS_VULKAN
#include "backend/vulkan_dispatch.h"
#include "ffx_api/vk/ffx_api_vk.h"
#endif
// The D3D12 backend's create description (ffx_api/dx12/ffx_api_dx12.h, which cannot share a file with the
// Vulkan header: both declare the frame generation swap chain's keys).
#define NR_FFX_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 0x0000002u
struct NrFfxCreateBackendDX12Desc {
  ffxCreateContextDescHeader header;
  void* device;  // ID3D12Device
};

namespace nr {

namespace {

uint32_t FfxFormat(nvrhi::Format f) {
  switch (f) {
    case nvrhi::Format::RGBA8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case nvrhi::Format::SRGBA8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB;
    case nvrhi::Format::BGRA8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    case nvrhi::Format::R10G10B10A2_UNORM: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    case nvrhi::Format::RGBA16_FLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case nvrhi::Format::R11G11B10_FLOAT: return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
    case nvrhi::Format::RG16_FLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case nvrhi::Format::R32_FLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case nvrhi::Format::R8_UNORM: return FFX_API_SURFACE_FORMAT_R8_UNORM;
    case nvrhi::Format::RGBA32_FLOAT: return FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
  }
}

uint32_t FfxQuality(UpscaleQuality q) {
  switch (q) {
    case UpscaleQuality::kNative: return FFX_UPSCALE_QUALITY_MODE_NATIVEAA;
    case UpscaleQuality::kQuality: return FFX_UPSCALE_QUALITY_MODE_QUALITY;
    case UpscaleQuality::kBalanced: return FFX_UPSCALE_QUALITY_MODE_BALANCED;
    case UpscaleQuality::kPerformance: return FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;
    case UpscaleQuality::kUltraPerformance: return FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE;
  }
  return FFX_UPSCALE_QUALITY_MODE_QUALITY;
}

#if NR_HAS_VULKAN
// FFX's Vulkan backend looks some functions up by their extension names (vkGetBufferMemoryRequirements2KHR)
// and calls them without a check. On a Vulkan 1.1+ device that has the function in core and did not
// enable the old extension, that name gives null: the core function is given instead.
PFN_vkVoidFunction VKAPI_CALL FfxGetDeviceProcAddr(VkDevice device, const char* name) {
  PFN_vkGetDeviceProcAddr real = VulkanGetDeviceProcAddr();
  if (!real || !name) return nullptr;
  if (PFN_vkVoidFunction f = real(device, name)) return f;
  const size_t n = std::strlen(name);
  for (const char* suffix : {"KHR", "EXT"}) {
    if (n > 3 && std::strcmp(name + n - 3, suffix) == 0) {
      const std::string core(name, n - 3);
      if (PFN_vkVoidFunction f = real(device, core.c_str())) return f;
    }
  }
  return nullptr;
}
#endif

void FfxMessage(uint32_t type, const wchar_t* message) {
  static int count = 0;
  if (++count > 50 || !message) return;
  std::string text;
  for (const wchar_t* p = message; *p; ++p) text += *p < 128 ? char(*p) : '?';
  Logf(type == FFX_API_MESSAGE_TYPE_ERROR ? LogLevel::kWarning : LogLevel::kInfo, "rexgpu-native: FSR: %s", text.c_str());
}

class Fsr3Upscaler final : public Upscaler {
 public:
  Fsr3Upscaler(nvrhi::IDevice* device, const UpscaleConfig& config, HMODULE dll, const char* dll_name)
      : device_(device), config_(config), dll_(dll), dll_name_(dll_name) {
    ffxLoadFunctions(&fn_, dll);
  }
  ~Fsr3Upscaler() override {
    if (context_) {
      device_->waitForIdle();
      fn_.DestroyContext(&context_, nullptr);
    }
  }

  bool ok() const { return fn_.CreateContext && fn_.DestroyContext && fn_.Dispatch && fn_.Query; }

  std::string Name() const override {
    return std::string(version_.empty() ? "FSR 3.1" : version_) +
           (device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN ? " (Vulkan)" : " (D3D12)");
  }

  UpscalerSize InputSize(UpscalerSize output) const override {
    // FSR's documented ratios (1, 1.5, 1.7, 2, 3), rounded as its own query rounds them.
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
    // The resources' states, through NVRHI, then its own work recorded, then FSR's.
    cl->clearState();
    cl->setTextureState(in.color, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    cl->setTextureState(in.depth, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    cl->setTextureState(in.motion, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    if (in.reactive) cl->setTextureState(in.reactive, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    cl->setTextureState(in.output, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    cl->commitBarriers();

    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.color = Resource(in.color, false);
    d.depth = Resource(in.depth, false);
    d.motionVectors = Resource(in.motion, false);
    if (in.reactive) d.reactive = Resource(in.reactive, false);
    d.output = Resource(in.output, true);
    // The picture's shift in input pixels (+x right, +y down), as the SDK's sample passes it.
    d.jitterOffset = {in.jitter_x, in.jitter_y};
    d.motionVectorScale = {1.0f, 1.0f};  // already input pixels, last frame minus this one
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
    // The game's picture is gamma-encoded LDR (8 bits a channel), not linear.
    d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    if (device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN) {
      d.commandList = cl->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer).pointer;
    } else {
      d.commandList = cl->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList).pointer;
    }
    const ffxReturnCode_t rc = fn_.Dispatch(&context_, &d.header);
    cl->clearState();
    if (rc != FFX_API_RETURN_OK) {
      if (failures_++ < 10) Logf(LogLevel::kWarning, "rexgpu-native: FSR dispatch failed (%u)", rc);
      return false;
    }
    return true;
  }

 private:
  FfxApiResource Resource(nvrhi::ITexture* t, bool output) const {
    const nvrhi::TextureDesc& td = t->getDesc();
    const uint32_t state = output ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    FfxApiResource r{};
    const bool vulkan = device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
    r.resource = t->getNativeObject(vulkan ? nvrhi::ObjectTypes::VK_Image : nvrhi::ObjectTypes::D3D12_Resource).pointer;
    r.state = state;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = FfxFormat(td.format);
    r.description.width = td.width;
    r.description.height = td.height;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = output ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    return r;
  }

  bool Create(UpscalerSize render, UpscalerSize output) {
    if (context_) {
      device_->waitForIdle();
      fn_.DestroyContext(&context_, nullptr);
      context_ = nullptr;
    }
    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    desc.maxRenderSize = {render.width, render.height};
    desc.maxUpscaleSize = {output.width, output.height};
    desc.fpMessage = FfxMessage;
    ffxReturnCode_t rc = FFX_API_RETURN_ERROR;
#if NR_HAS_VULKAN
    if (device_->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN) {
      ffxCreateBackendVKDesc backend{};
      backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
      backend.vkDevice = static_cast<VkDevice>(device_->getNativeObject(nvrhi::ObjectTypes::VK_Device).pointer);
      backend.vkPhysicalDevice =
          static_cast<VkPhysicalDevice>(device_->getNativeObject(nvrhi::ObjectTypes::VK_PhysicalDevice).pointer);
      backend.vkDeviceProcAddr = FfxGetDeviceProcAddr;
      desc.header.pNext = &backend.header;
      rc = fn_.CreateContext(&context_, &desc.header, nullptr);
    }
#endif
#if NR_HAS_D3D12
    if (device_->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12) {
      NrFfxCreateBackendDX12Desc backend{};
      backend.header.type = NR_FFX_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
      backend.device = device_->getNativeObject(nvrhi::ObjectTypes::D3D12_Device).pointer;
      desc.header.pNext = &backend.header;
      rc = fn_.CreateContext(&context_, &desc.header, nullptr);
    }
#endif
    if (rc != FFX_API_RETURN_OK || !context_) {
      Logf(LogLevel::kWarning, "rexgpu-native: FSR context not created (%u) for %ux%u to %ux%u", rc, render.width,
           render.height, output.width, output.height);
      context_ = nullptr;
      return false;
    }
    max_render_ = render;
    max_output_ = output;
    ffxQueryGetProviderVersion v{};
    v.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (fn_.Query(&context_, &v.header) == FFX_API_RETURN_OK && v.versionName) version_ = std::string("FSR ") + v.versionName;
    Logf(LogLevel::kInfo, "rexgpu-native: %s context for %ux%u to %ux%u (%s)", Name().c_str(), render.width, render.height,
         output.width, output.height, dll_name_);
    return true;
  }

  nvrhi::IDevice* device_;
  UpscaleConfig config_;
  HMODULE dll_;
  const char* dll_name_;
  ffxFunctions fn_{};
  ffxContext context_ = nullptr;
  UpscalerSize max_render_, max_output_;
  std::string version_;
  std::chrono::steady_clock::time_point last_{};
  uint32_t failures_ = 0;
};

}  // namespace

std::unique_ptr<Upscaler> CreateFsr3Upscaler(nvrhi::IDevice* device, const UpscaleConfig& config, std::string* why) {
  const bool vulkan = device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
  const wchar_t* name = vulkan ? L"amd_fidelityfx_vk.dll" : L"amd_fidelityfx_dx12.dll";
  const char* name8 = vulkan ? "amd_fidelityfx_vk.dll" : "amd_fidelityfx_dx12.dll";
  const std::wstring path = UpscalerDllFolder() + name;
  HMODULE dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!dll) {
    if (why) *why = std::string(name8) + " is not beside the game";
    return nullptr;
  }
  auto u = std::make_unique<Fsr3Upscaler>(device, config, dll, name8);
  if (!u->ok()) {
    if (why) *why = std::string(name8) + " has no FidelityFX API";
    return nullptr;
  }
  return u;
}

}  // namespace nr

#else  // !NR_HAS_FFX

namespace nr {
std::unique_ptr<Upscaler> CreateFsr3Upscaler(nvrhi::IDevice*, const UpscaleConfig&, std::string* why) {
  if (why) *why = "not built in";
  return nullptr;
}
}  // namespace nr

#endif
