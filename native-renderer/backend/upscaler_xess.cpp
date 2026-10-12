// Intel XeSS Super Resolution (upscaler.h, docs/upscalers.md).
//
// The XeSS SDK 3.0.2: libxess.dll, loaded at run time from the game's folder (the SDK's headers give
// only the types here: XESS_TYPES_ONLY). XeSS runs on any GPU with the shader features it needs (Intel
// Arc's XMX units, others through DP4a), on Vulkan and D3D12. The quality modes keep the other
// upscalers' ratios (1.5, 1.7, 2, 3: XeSS's own "legacy" scale factors); native is XeSS's anti-aliasing
// mode (XESS_QUALITY_SETTING_AA).
//
// The context is made with the upscaler and its pipelines built in the background; it is initialised
// for the sizes on the first upscale (a blocking call: XeSS allocates and finishes its pipelines then).
// Like FSR and DLSS, XeSS records into the game renderer's open command list with the inputs in
// shader-read state and the output in unordered-access state, and NVRHI's cached state is cleared after.
// On Vulkan the game's instance and device must have XeSS's extensions and features
// (upscaler_vulkan.h).

#include "backend/upscaler.h"
#if NR_HAS_VULKAN
#include "backend/upscaler_vulkan.h"
#endif

#if NR_HAS_XESS

#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "backend/log.h"
#if NR_HAS_VULKAN
#include <nvrhi/vulkan.h>
#include <vulkan/vulkan_core.h>
#endif

#define XESS_TYPES_ONLY
#include <xess/xess.h>
#if NR_HAS_VULKAN
#include <xess/xess_vk.h>
#endif
#if NR_HAS_D3D12
#include <xess/xess_d3d12.h>
#endif

namespace nr {

namespace {

constexpr wchar_t kDllName[] = L"libxess.dll";

// libxess.dll's functions this file uses.
struct XessFunctions {
  HMODULE dll = nullptr;
  xess_result_t (*GetVersion)(xess_version_t*) = nullptr;
  xess_result_t (*GetOptimalInputResolution)(xess_context_handle_t, const xess_2d_t*, xess_quality_settings_t,
                                             xess_2d_t*, xess_2d_t*, xess_2d_t*) = nullptr;
  xess_result_t (*DestroyContext)(xess_context_handle_t) = nullptr;
  xess_result_t (*SetJitterScale)(xess_context_handle_t, float, float) = nullptr;
  xess_result_t (*SetVelocityScale)(xess_context_handle_t, float, float) = nullptr;
  xess_result_t (*SetLoggingCallback)(xess_context_handle_t, xess_logging_level_t, xess_app_log_callback_t) = nullptr;
  xess_result_t (*ForceLegacyScaleFactors)(xess_context_handle_t, bool) = nullptr;
  xess_result_t (*SetMaxResponsiveMaskValue)(xess_context_handle_t, float) = nullptr;
  xess_result_t (*IsOptimalDriver)(xess_context_handle_t) = nullptr;
#if NR_HAS_VULKAN
  xess_result_t (*VKGetRequiredInstanceExtensions)(uint32_t*, const char* const**, uint32_t*) = nullptr;
  xess_result_t (*VKGetRequiredDeviceExtensions)(VkInstance, VkPhysicalDevice, uint32_t*, const char* const**) = nullptr;
  xess_result_t (*VKGetRequiredDeviceFeatures)(VkInstance, VkPhysicalDevice, void**) = nullptr;
  xess_result_t (*VKCreateContext)(VkInstance, VkPhysicalDevice, VkDevice, xess_context_handle_t*) = nullptr;
  xess_result_t (*VKBuildPipelines)(xess_context_handle_t, VkPipelineCache, bool, uint32_t) = nullptr;
  xess_result_t (*VKInit)(xess_context_handle_t, const xess_vk_init_params_t*) = nullptr;
  xess_result_t (*VKExecute)(xess_context_handle_t, VkCommandBuffer, const xess_vk_execute_params_t*) = nullptr;
#endif
#if NR_HAS_D3D12
  xess_result_t (*D3D12CreateContext)(ID3D12Device*, xess_context_handle_t*) = nullptr;
  xess_result_t (*D3D12BuildPipelines)(xess_context_handle_t, ID3D12PipelineLibrary*, bool, uint32_t) = nullptr;
  xess_result_t (*D3D12Init)(xess_context_handle_t, const xess_d3d12_init_params_t*) = nullptr;
  xess_result_t (*D3D12Execute)(xess_context_handle_t, ID3D12GraphicsCommandList*, const xess_d3d12_execute_params_t*) =
      nullptr;
#endif
};

// The DLL, loaded once (and kept: XeSS's own threads may outlive a context).
const XessFunctions* Xess() {
  static const XessFunctions fns = [] {
    XessFunctions f;
    const std::wstring path = UpscalerDllFolder() + kDllName;
    f.dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!f.dll) return f;
    auto get = [&](auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(f.dll, name)); };
    get(f.GetVersion, "xessGetVersion");
    get(f.GetOptimalInputResolution, "xessGetOptimalInputResolution");
    get(f.DestroyContext, "xessDestroyContext");
    get(f.SetJitterScale, "xessSetJitterScale");
    get(f.SetVelocityScale, "xessSetVelocityScale");
    get(f.SetLoggingCallback, "xessSetLoggingCallback");
    get(f.ForceLegacyScaleFactors, "xessForceLegacyScaleFactors");
    get(f.SetMaxResponsiveMaskValue, "xessSetMaxResponsiveMaskValue");
    get(f.IsOptimalDriver, "xessIsOptimalDriver");
#if NR_HAS_VULKAN
    get(f.VKGetRequiredInstanceExtensions, "xessVKGetRequiredInstanceExtensions");
    get(f.VKGetRequiredDeviceExtensions, "xessVKGetRequiredDeviceExtensions");
    get(f.VKGetRequiredDeviceFeatures, "xessVKGetRequiredDeviceFeatures");
    get(f.VKCreateContext, "xessVKCreateContext");
    get(f.VKBuildPipelines, "xessVKBuildPipelines");
    get(f.VKInit, "xessVKInit");
    get(f.VKExecute, "xessVKExecute");
#endif
#if NR_HAS_D3D12
    get(f.D3D12CreateContext, "xessD3D12CreateContext");
    get(f.D3D12BuildPipelines, "xessD3D12BuildPipelines");
    get(f.D3D12Init, "xessD3D12Init");
    get(f.D3D12Execute, "xessD3D12Execute");
#endif
    return f;
  }();
  return fns.dll ? &fns : nullptr;
}

std::string VersionText(const XessFunctions* x) {
  xess_version_t v{};
  if (!x->GetVersion || x->GetVersion(&v) != XESS_RESULT_SUCCESS) return "XeSS";
  char buf[48];
  std::snprintf(buf, sizeof(buf), "XeSS %u.%u.%u", v.major, v.minor, v.patch);
  return buf;
}

void XessLog(const char* message, xess_logging_level_t level) {
  static int count = 0;
  if (!message || ++count > 100) return;
  Logf(level >= XESS_LOGGING_LEVEL_WARNING ? LogLevel::kWarning : LogLevel::kInfo, "rexgpu-native: XeSS: %s", message);
}

xess_quality_settings_t Quality(UpscaleQuality q) {
  // With the legacy scale factors: ULTRA_QUALITY 1.5, QUALITY 1.7, PERFORMANCE 2.0, ULTRA_PERFORMANCE 3.0.
  switch (q) {
    case UpscaleQuality::kNative: return XESS_QUALITY_SETTING_AA;
    case UpscaleQuality::kQuality: return XESS_QUALITY_SETTING_ULTRA_QUALITY;
    case UpscaleQuality::kBalanced: return XESS_QUALITY_SETTING_QUALITY;
    case UpscaleQuality::kPerformance: return XESS_QUALITY_SETTING_PERFORMANCE;
    case UpscaleQuality::kUltraPerformance: return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
  }
  return XESS_QUALITY_SETTING_ULTRA_QUALITY;
}

const char* ResultName(xess_result_t r) {
  switch (r) {
    case XESS_RESULT_SUCCESS: return "success";
    case XESS_RESULT_WARNING_OLD_DRIVER: return "old driver";
    case XESS_RESULT_ERROR_UNSUPPORTED_DEVICE: return "unsupported device";
    case XESS_RESULT_ERROR_UNSUPPORTED_DRIVER: return "unsupported driver";
    case XESS_RESULT_ERROR_UNINITIALIZED: return "uninitialised";
    case XESS_RESULT_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY: return "out of GPU memory";
    case XESS_RESULT_ERROR_DEVICE: return "device error";
    case XESS_RESULT_ERROR_NOT_IMPLEMENTED: return "not implemented";
    case XESS_RESULT_ERROR_INVALID_CONTEXT: return "invalid context";
    case XESS_RESULT_ERROR_UNSUPPORTED: return "unsupported";
    case XESS_RESULT_ERROR_CANT_LOAD_LIBRARY: return "a library could not be loaded";
    case XESS_RESULT_ERROR_WRONG_CALL_ORDER: return "wrong call order";
    default: return "error";
  }
}

class XessUpscaler final : public Upscaler {
 public:
  XessUpscaler(nvrhi::IDevice* device, const UpscaleConfig& config, const XessFunctions* x, xess_context_handle_t ctx)
      : device_(device), config_(config), x_(x), ctx_(ctx), version_(VersionText(x)) {
    vulkan_ = device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
  }
  ~XessUpscaler() override {
    if (ctx_) {
      device_->waitForIdle();
      x_->DestroyContext(ctx_);
    }
  }

  // The flags of the upscale (fixed for the context: the reactive mask is given when the setting has one).
  static uint32_t Flags(const UpscaleConfig& config) {
    uint32_t f = XESS_INIT_FLAG_LDR_INPUT_COLOR;  // gamma-encoded LDR, as the game draws it
    if (config.reactive > 0.0f) f |= XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK;
    return f;
  }

  std::string Name() const override { return version_ + (vulkan_ ? " (Vulkan)" : " (D3D12)"); }

  UpscalerSize InputSize(UpscalerSize output) const override {
    const xess_2d_t out{output.width, output.height};
    xess_2d_t optimal{}, lo{}, hi{};
    if (x_->GetOptimalInputResolution &&
        x_->GetOptimalInputResolution(ctx_, &out, Quality(config_.quality), &optimal, &lo, &hi) == XESS_RESULT_SUCCESS &&
        optimal.x > 0 && optimal.y > 0) {
      return {optimal.x, optimal.y};
    }
    const float ratio = DefaultUpscaleRatio(config_.quality);
    return {UpscaleInputSize(output.width, ratio), UpscaleInputSize(output.height, ratio)};
  }

  bool Evaluate(nvrhi::ICommandList* cl, const UpscalerInputs& in) override {
    if (!in.color || !in.depth || !in.motion || !in.output) return false;
    if (in.output_size.width != output_.width || in.output_size.height != output_.height) {
      if (!Init(in.output_size)) return false;
    }
    cl->clearState();
    cl->setTextureState(in.color, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    cl->setTextureState(in.depth, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    cl->setTextureState(in.motion, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    if (in.reactive) cl->setTextureState(in.reactive, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    cl->setTextureState(in.output, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    cl->commitBarriers();
    xess_result_t r = XESS_RESULT_ERROR_UNKNOWN;
    const bool mask = (Flags(config_) & XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK) && in.reactive;
#if NR_HAS_VULKAN
    if (vulkan_) {
      xess_vk_execute_params_t e{};
      e.colorTexture = VkView(in.color);
      e.velocityTexture = VkView(in.motion);
      e.depthTexture = VkView(in.depth);
      if (mask) e.responsivePixelMaskTexture = VkView(in.reactive);
      e.outputTexture = VkView(in.output);
      // The picture's shift in input pixels (+x right, +y down), and last frame's position minus this
      // one's in input pixels (XeSS's conventions: its guide, "Jitter" and "Motion Vectors").
      e.jitterOffsetX = in.jitter_x;
      e.jitterOffsetY = in.jitter_y;
      e.exposureScale = 1.0f;
      e.resetHistory = in.reset ? 1u : 0u;
      e.inputWidth = in.render.width;
      e.inputHeight = in.render.height;
      auto cb = static_cast<VkCommandBuffer>(cl->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer).pointer);
      r = x_->VKExecute(ctx_, cb, &e);
    }
#endif
#if NR_HAS_D3D12
    if (!vulkan_) {
      auto res = [](nvrhi::ITexture* t) {
        return t ? static_cast<ID3D12Resource*>(t->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource).pointer) : nullptr;
      };
      xess_d3d12_execute_params_t e{};
      e.pColorTexture = res(in.color);
      e.pVelocityTexture = res(in.motion);
      e.pDepthTexture = res(in.depth);
      e.pResponsivePixelMaskTexture = mask ? res(in.reactive) : nullptr;
      e.pOutputTexture = res(in.output);
      e.jitterOffsetX = in.jitter_x;
      e.jitterOffsetY = in.jitter_y;
      e.exposureScale = 1.0f;
      e.resetHistory = in.reset ? 1u : 0u;
      e.inputWidth = in.render.width;
      e.inputHeight = in.render.height;
      auto* d3d_cl =
          static_cast<ID3D12GraphicsCommandList*>(cl->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList).pointer);
      r = x_->D3D12Execute(ctx_, d3d_cl, &e);
    }
#endif
    cl->clearState();
    if (r != XESS_RESULT_SUCCESS) {
      if (failures_++ < 10) Logf(LogLevel::kWarning, "rexgpu-native: XeSS execution failed (%s)", ResultName(r));
      return false;
    }
    return true;
  }

 private:
#if NR_HAS_VULKAN
  xess_vk_image_view_info VkView(nvrhi::ITexture* t) const {
    const nvrhi::TextureDesc& d = t->getDesc();
    xess_vk_image_view_info v{};
    v.imageView = static_cast<VkImageView>(
        t->getNativeView(nvrhi::ObjectTypes::VK_ImageView, d.format, nvrhi::AllSubresources, nvrhi::TextureDimension::Texture2D)
            .pointer);
    v.image = static_cast<VkImage>(t->getNativeObject(nvrhi::ObjectTypes::VK_Image).pointer);
    v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    v.format = static_cast<VkFormat>(nvrhi::vulkan::convertFormat(d.format));
    v.width = d.width;
    v.height = d.height;
    return v;
  }
#endif

  bool Init(UpscalerSize output) {
    // XeSS's initialisation must not overlap work in flight that uses its resources.
    device_->waitForIdle();
    xess_result_t r = XESS_RESULT_ERROR_UNKNOWN;
#if NR_HAS_VULKAN
    if (vulkan_) {
      xess_vk_init_params_t p{};
      p.outputResolution = {output.width, output.height};
      p.qualitySetting = Quality(config_.quality);
      p.initFlags = Flags(config_);
      r = x_->VKInit(ctx_, &p);
    }
#endif
#if NR_HAS_D3D12
    if (!vulkan_) {
      xess_d3d12_init_params_t p{};
      p.outputResolution = {output.width, output.height};
      p.qualitySetting = Quality(config_.quality);
      p.initFlags = Flags(config_);
      r = x_->D3D12Init(ctx_, &p);
    }
#endif
    if (r != XESS_RESULT_SUCCESS && r != XESS_RESULT_WARNING_OLD_DRIVER) {
      if (failures_++ < 10) {
        Logf(LogLevel::kWarning, "rexgpu-native: XeSS not initialised (%s) for %ux%u", ResultName(r), output.width,
             output.height);
      }
      output_ = {};
      return false;
    }
    output_ = output;
    if (x_->SetJitterScale) x_->SetJitterScale(ctx_, 1.0f, 1.0f);
    if (x_->SetVelocityScale) x_->SetVelocityScale(ctx_, 1.0f, 1.0f);
    Logf(LogLevel::kInfo, "rexgpu-native: %s for %ux%u%s", Name().c_str(), output.width, output.height,
         r == XESS_RESULT_WARNING_OLD_DRIVER ? " (XeSS says the driver is old)" : "");
    return true;
  }

  nvrhi::IDevice* device_;
  UpscaleConfig config_;
  const XessFunctions* x_;
  xess_context_handle_t ctx_ = nullptr;
  std::string version_;
  bool vulkan_ = true;
  UpscalerSize output_;
  uint32_t failures_ = 0;
};

}  // namespace

std::unique_ptr<Upscaler> CreateXessUpscaler(nvrhi::IDevice* device, const UpscaleConfig& config, std::string* why) {
  const XessFunctions* x = Xess();
  if (!x) {
    if (why) *why = "libxess.dll is not beside the game";
    return nullptr;
  }
  if (!x->DestroyContext || !x->GetOptimalInputResolution) {
    if (why) *why = "libxess.dll lacks the XeSS functions";
    return nullptr;
  }
  const bool vulkan = device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
  xess_context_handle_t ctx = nullptr;
  xess_result_t r = XESS_RESULT_ERROR_UNKNOWN;
  const uint32_t flags = XessUpscaler::Flags(config);
#if NR_HAS_VULKAN
  if (vulkan && x->VKCreateContext && x->VKInit && x->VKExecute) {
    r = x->VKCreateContext(
        static_cast<VkInstance>(device->getNativeObject(nvrhi::ObjectTypes::VK_Instance).pointer),
        static_cast<VkPhysicalDevice>(device->getNativeObject(nvrhi::ObjectTypes::VK_PhysicalDevice).pointer),
        static_cast<VkDevice>(device->getNativeObject(nvrhi::ObjectTypes::VK_Device).pointer), &ctx);
  }
#endif
#if NR_HAS_D3D12
  if (!vulkan && x->D3D12CreateContext && x->D3D12Init && x->D3D12Execute) {
    r = x->D3D12CreateContext(static_cast<ID3D12Device*>(device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device).pointer),
                              &ctx);
  }
#endif
  if (r != XESS_RESULT_SUCCESS || !ctx) {
    if (why) *why = std::string("XeSS cannot run on this GPU or driver (") + ResultName(r) + ")";
    return nullptr;
  }
  if (x->SetLoggingCallback) x->SetLoggingCallback(ctx, XESS_LOGGING_LEVEL_WARNING, XessLog);
  if (x->ForceLegacyScaleFactors) x->ForceLegacyScaleFactors(ctx, true);
  // Pipelines in the background now; the first upscale's initialisation waits for them.
  xess_result_t b = XESS_RESULT_SUCCESS;
#if NR_HAS_VULKAN
  if (vulkan && x->VKBuildPipelines) b = x->VKBuildPipelines(ctx, VK_NULL_HANDLE, false, flags);
#endif
#if NR_HAS_D3D12
  if (!vulkan && x->D3D12BuildPipelines) b = x->D3D12BuildPipelines(ctx, nullptr, false, flags);
#endif
  if (b != XESS_RESULT_SUCCESS) {
    x->DestroyContext(ctx);
    if (why) *why = std::string("XeSS could not build its pipelines (") + ResultName(b) + ")";
    return nullptr;
  }
  return std::make_unique<XessUpscaler>(device, config, x, ctx);
}

#if NR_HAS_VULKAN
std::vector<std::string> XessVulkanInstanceExtensions() {
  std::vector<std::string> out;
  const XessFunctions* x = Xess();
  if (!x || !x->VKGetRequiredInstanceExtensions) return out;
  uint32_t count = 0, api = 0;
  const char* const* names = nullptr;
  if (x->VKGetRequiredInstanceExtensions(&count, &names, &api) == XESS_RESULT_SUCCESS && names) {
    for (uint32_t i = 0; i < count; ++i) out.emplace_back(names[i]);
  }
  return out;
}

namespace {

// The size of the feature structures XeSS asks for (the chain it returns has no sizes).
size_t FeatureStructSize(VkStructureType type) {
  switch (type) {
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2: return sizeof(VkPhysicalDeviceFeatures2);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: return sizeof(VkPhysicalDeviceVulkan11Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: return sizeof(VkPhysicalDeviceVulkan12Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: return sizeof(VkPhysicalDeviceVulkan13Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT:
      return sizeof(VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES:
      return sizeof(VkPhysicalDeviceShaderIntegerDotProductFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES: return sizeof(VkPhysicalDeviceShaderFloat16Int8Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES: return sizeof(VkPhysicalDevice16BitStorageFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES: return sizeof(VkPhysicalDevice8BitStorageFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES:
      return sizeof(VkPhysicalDeviceSubgroupSizeControlFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES:
      return sizeof(VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
      return sizeof(VkPhysicalDeviceDescriptorIndexingFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
      return sizeof(VkPhysicalDeviceBufferDeviceAddressFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES:
      return sizeof(VkPhysicalDeviceTimelineSemaphoreFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES: return sizeof(VkPhysicalDeviceSynchronization2Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES: return sizeof(VkPhysicalDeviceDynamicRenderingFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES: return sizeof(VkPhysicalDeviceMaintenance4Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES: return sizeof(VkPhysicalDeviceScalarBlockLayoutFeatures);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES: return sizeof(VkPhysicalDeviceHostQueryResetFeatures);
    default: return 0;
  }
}

// Promoted structures as their Vulkan 1.2 / 1.3 members, so the device's chain does not carry both a
// VkPhysicalDeviceVulkan12Features and a structure it covers (the specification forbids that).
bool Promote(const uint8_t* s, VkStructureType type, VkPhysicalDeviceVulkan12Features& v12, VkPhysicalDeviceVulkan13Features& v13,
             bool& used12, bool& used13) {
  switch (type) {
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES: {
      auto& f = *reinterpret_cast<const VkPhysicalDeviceShaderFloat16Int8Features*>(s);
      v12.shaderFloat16 |= f.shaderFloat16;
      v12.shaderInt8 |= f.shaderInt8;
      used12 = true;
      return true;
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES: {
      auto& f = *reinterpret_cast<const VkPhysicalDevice8BitStorageFeatures*>(s);
      v12.storageBuffer8BitAccess |= f.storageBuffer8BitAccess;
      v12.uniformAndStorageBuffer8BitAccess |= f.uniformAndStorageBuffer8BitAccess;
      v12.storagePushConstant8 |= f.storagePushConstant8;
      used12 = true;
      return true;
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES: {
      v12.shaderSubgroupExtendedTypes |=
          reinterpret_cast<const VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures*>(s)->shaderSubgroupExtendedTypes;
      used12 = true;
      return true;
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES: {
      v12.bufferDeviceAddress |= reinterpret_cast<const VkPhysicalDeviceBufferDeviceAddressFeatures*>(s)->bufferDeviceAddress;
      used12 = true;
      return true;
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES: {
      v12.timelineSemaphore |= reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures*>(s)->timelineSemaphore;
      used12 = true;
      return true;
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES: {
      v12.scalarBlockLayout |= reinterpret_cast<const VkPhysicalDeviceScalarBlockLayoutFeatures*>(s)->scalarBlockLayout;
      used12 = true;
      return true;
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES: {
      v12.hostQueryReset |= reinterpret_cast<const VkPhysicalDeviceHostQueryResetFeatures*>(s)->hostQueryReset;
      used12 = true;
      return true;
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES: {
      v13.shaderIntegerDotProduct |=
          reinterpret_cast<const VkPhysicalDeviceShaderIntegerDotProductFeatures*>(s)->shaderIntegerDotProduct;
      used13 = true;
      return true;
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES: {
      auto& f = *reinterpret_cast<const VkPhysicalDeviceSubgroupSizeControlFeatures*>(s);
      v13.subgroupSizeControl |= f.subgroupSizeControl;
      v13.computeFullSubgroups |= f.computeFullSubgroups;
      used13 = true;
      return true;
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES: {
      v13.synchronization2 |= reinterpret_cast<const VkPhysicalDeviceSynchronization2Features*>(s)->synchronization2;
      used13 = true;
      return true;
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES: {
      v13.dynamicRendering |= reinterpret_cast<const VkPhysicalDeviceDynamicRenderingFeatures*>(s)->dynamicRendering;
      used13 = true;
      return true;
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES: {
      v13.maintenance4 |= reinterpret_cast<const VkPhysicalDeviceMaintenance4Features*>(s)->maintenance4;
      used13 = true;
      return true;
    }
    default: return false;
  }
}

template <typename T>
std::vector<uint8_t> Bytes(const T& s) {
  const auto* b = reinterpret_cast<const uint8_t*>(&s);
  return std::vector<uint8_t>(b, b + sizeof(T));
}

}  // namespace

UpscalerVulkanNeeds XessVulkanDeviceNeeds(VkInstance instance, VkPhysicalDevice physical) {
  UpscalerVulkanNeeds out;
  const XessFunctions* x = Xess();
  if (!x || !x->VKGetRequiredDeviceExtensions || !x->VKGetRequiredDeviceFeatures) return out;
  uint32_t count = 0;
  const char* const* names = nullptr;
  if (x->VKGetRequiredDeviceExtensions(instance, physical, &count, &names) == XESS_RESULT_SUCCESS && names) {
    for (uint32_t i = 0; i < count; ++i) out.extensions.emplace_back(names[i]);
  }
  void* chain = nullptr;
  if (x->VKGetRequiredDeviceFeatures(instance, physical, &chain) != XESS_RESULT_SUCCESS) return out;
  VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  bool used12 = false, used13 = false;
  for (auto* p = static_cast<const VkBaseInStructure*>(chain); p; p = p->pNext) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(p);
    if (Promote(bytes, p->sType, v12, v13, used12, used13)) continue;
    const size_t size = FeatureStructSize(p->sType);
    if (!size) {
      Logf(LogLevel::kWarning, "rexgpu-native: XeSS asks for a Vulkan feature structure this build does not know (%d)",
           int(p->sType));
      continue;
    }
    std::vector<uint8_t> copy(bytes, bytes + size);
    reinterpret_cast<VkBaseOutStructure*>(copy.data())->pNext = nullptr;
    out.features.push_back(std::move(copy));
  }
  if (used12) out.features.push_back(Bytes(v12));
  if (used13) out.features.push_back(Bytes(v13));
  return out;
}
#endif  // NR_HAS_VULKAN

}  // namespace nr

#else  // !NR_HAS_XESS

namespace nr {
std::unique_ptr<Upscaler> CreateXessUpscaler(nvrhi::IDevice*, const UpscaleConfig&, std::string* why) {
  if (why) *why = "not built in (the XeSS SDK was not found at build time: NR_XESS_SDK_DIR)";
  return nullptr;
}
#if NR_HAS_VULKAN
std::vector<std::string> XessVulkanInstanceExtensions() { return {}; }
UpscalerVulkanNeeds XessVulkanDeviceNeeds(VkInstance, VkPhysicalDevice) { return {}; }
#endif
}  // namespace nr

#endif
