// NVIDIA DLSS Super Resolution and DLAA (upscaler.h, docs/upscalers.md).
//
// NGX from the DLSS SDK 310.9.1: the SDK's loader library (nvsdk_ngx_d.lib, linked in) loads
// nvngx_dlss.dll from the game's folder. DLSS runs on NVIDIA RTX GPUs. The quality modes are NGX's
// (native: DLAA, the same size in and out); --native_upscaler_dlss_preset asks for a render preset
// (J or K: DLSS 4, the transformer model; L or M: DLSS 4.5), "default" leaves the choice to the DLL.
//
// NGX is initialised once per device (a project ID for a custom engine: the game has no NVIDIA
// application ID) and shut down with the last upscaler. On Vulkan the game's instance and device must
// have been created with NGX's extensions (upscaler_vulkan.h). Like FSR, DLSS records into the game
// renderer's open command list: the inputs are put in shader-read state and the output in
// unordered-access state through NVRHI, and NVRHI's cached state is cleared afterwards.

#include "backend/upscaler.h"
#if NR_HAS_VULKAN
#include "backend/upscaler_vulkan.h"
#endif

#if NR_HAS_DLSS

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

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

#include "backend/vulkan_dispatch.h"
#endif
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#if NR_HAS_VULKAN
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_vk.h>
#endif

namespace nr {

namespace {

// This game's project ID for NGX (a custom engine), and the engine's version.
constexpr char kProjectId[] = "f059f07d-9843-4ea5-a360-2d28c78d1bd9";
constexpr char kEngineVersion[] = "1.0";
constexpr wchar_t kDllName[] = L"nvngx_dlss.dll";

bool DllPresent() {
  const std::wstring path = UpscalerDllFolder() + kDllName;
  const DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// NGX's own files (its log when on): a folder in the user's temporary folder.
const wchar_t* DataPath() {
  static const std::wstring path = [] {
    wchar_t temp[MAX_PATH + 1] = {};
    const DWORD n = GetTempPathW(MAX_PATH + 1, temp);
    std::wstring p = std::wstring(temp, n) + L"kk-native-ngx";
    CreateDirectoryW(p.c_str(), nullptr);
    return p;
  }();
  return path.c_str();
}

std::atomic<bool> g_ngx_log{false};

void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
  static std::atomic<int> count{0};
  if (!message || ++count > 200) return;
  std::string s(message);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  Logf(LogLevel::kInfo, "rexgpu-native: NGX: %s", s.c_str());
}

// Where NGX looks for nvngx_dlss.dll besides the executable's folder (the same folder: the tests'
// executables and the game both keep it there), and NGX's logging (off unless the upscaler's per-frame
// log is on: --native_upscaler_debug=16).
const NVSDK_NGX_FeatureCommonInfo* CommonInfo() {
  static const std::wstring folder = UpscalerDllFolder();
  static const wchar_t* paths[1] = {folder.c_str()};
  static NVSDK_NGX_FeatureCommonInfo info{};
  info.PathListInfo.Path = paths;
  info.PathListInfo.Length = folder.empty() ? 0 : 1;
  info.LoggingInfo.LoggingCallback = NgxLog;
  info.LoggingInfo.MinimumLoggingLevel = g_ngx_log ? NVSDK_NGX_LOGGING_LEVEL_ON : NVSDK_NGX_LOGGING_LEVEL_OFF;
  info.LoggingInfo.DisableOtherLoggingSinks = true;
  return &info;
}

NVSDK_NGX_FeatureDiscoveryInfo Discovery() {
  NVSDK_NGX_FeatureDiscoveryInfo d{};
  d.SDKVersion = NVSDK_NGX_Version_API;
  d.FeatureID = NVSDK_NGX_Feature_SuperSampling;
  d.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
  d.Identifier.v.ProjectDesc.ProjectId = kProjectId;
  d.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
  d.Identifier.v.ProjectDesc.EngineVersion = kEngineVersion;
  d.ApplicationDataPath = DataPath();
  d.FeatureInfo = CommonInfo();
  return d;
}

std::string ResultText(NVSDK_NGX_Result r) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(r));
  return buf;
}

// The DLL's file version ("310.4.0").
std::string DllVersion() {
  const std::wstring path = UpscalerDllFolder() + kDllName;
  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
  if (!size) return {};
  std::string data(size, '\0');
  if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
  VS_FIXEDFILEINFO* info = nullptr;
  UINT len = 0;
  if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) return {};
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                HIWORD(info->dwFileVersionLS));
  return buf;
}

// NGX, once per device, shared by the upscalers on it.
struct NgxSession {
  void* native_device = nullptr;
  bool vulkan = false;
  int refs = 0;
  NVSDK_NGX_Parameter* caps = nullptr;
};
std::mutex g_ngx_mutex;
NgxSession g_ngx;

void ShutdownLocked() {
  if (g_ngx.vulkan) {
#if NR_HAS_VULKAN
    if (g_ngx.caps) NVSDK_NGX_VULKAN_DestroyParameters(g_ngx.caps);
    NVSDK_NGX_VULKAN_Shutdown1(static_cast<VkDevice>(g_ngx.native_device));
#endif
  } else {
    if (g_ngx.caps) NVSDK_NGX_D3D12_DestroyParameters(g_ngx.caps);
    NVSDK_NGX_D3D12_Shutdown1(static_cast<ID3D12Device*>(g_ngx.native_device));
  }
  g_ngx = NgxSession{};
}

NVSDK_NGX_Parameter* AcquireNgx(nvrhi::IDevice* device, std::string* why) {
  std::lock_guard lock(g_ngx_mutex);
  const bool vulkan = device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
  void* native = device->getNativeObject(vulkan ? nvrhi::ObjectTypes::VK_Device : nvrhi::ObjectTypes::D3D12_Device).pointer;
  if (g_ngx.refs > 0) {
    if (g_ngx.native_device != native) {
      if (why) *why = "NGX is in use on another device";
      return nullptr;
    }
    ++g_ngx.refs;
    return g_ngx.caps;
  }
  NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
  if (vulkan) {
#if NR_HAS_VULKAN
    auto instance = static_cast<VkInstance>(device->getNativeObject(nvrhi::ObjectTypes::VK_Instance).pointer);
    auto physical = static_cast<VkPhysicalDevice>(device->getNativeObject(nvrhi::ObjectTypes::VK_PhysicalDevice).pointer);
    r = NVSDK_NGX_VULKAN_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, DataPath(), instance,
                                             physical, static_cast<VkDevice>(native), VulkanGetInstanceProcAddr(),
                                             VulkanGetDeviceProcAddr(), CommonInfo(), NVSDK_NGX_Version_API);
#endif
  } else {
    r = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, DataPath(),
                                            static_cast<ID3D12Device*>(native), CommonInfo(), NVSDK_NGX_Version_API);
  }
  if (NVSDK_NGX_FAILED(r)) {
    if (why) *why = "NGX did not start (" + ResultText(r) + ")";
    return nullptr;
  }
  g_ngx.native_device = native;
  g_ngx.vulkan = vulkan;
  g_ngx.refs = 1;
  NVSDK_NGX_Parameter* caps = nullptr;
#if NR_HAS_VULKAN
  if (vulkan) r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&caps);
#endif
  if (!vulkan) r = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
  g_ngx.caps = caps;
  int available = 0, needs_driver = 0;
  unsigned driver_major = 0, driver_minor = 0;
  int init_result = 0;
  if (NVSDK_NGX_SUCCEED(r) && caps) {
    NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &driver_major);
    NVSDK_NGX_Parameter_GetUI(caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &driver_minor);
    NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init_result);
  }
  if (!available) {
    if (why) {
      if (needs_driver) {
        *why = "DLSS needs a newer NVIDIA driver (" + std::to_string(driver_major) + "." + std::to_string(driver_minor) +
               " or later)";
      } else {
        *why = "DLSS is not available on this GPU (an NVIDIA RTX GPU is needed; NGX " +
               ResultText(static_cast<NVSDK_NGX_Result>(init_result)) + ")";
      }
    }
    ShutdownLocked();
    return nullptr;
  }
  return caps;
}

void ReleaseNgx() {
  std::lock_guard lock(g_ngx_mutex);
  if (g_ngx.refs > 0 && --g_ngx.refs == 0) ShutdownLocked();
}

NVSDK_NGX_PerfQuality_Value PerfQuality(UpscaleQuality q) {
  switch (q) {
    case UpscaleQuality::kNative: return NVSDK_NGX_PerfQuality_Value_DLAA;
    case UpscaleQuality::kQuality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case UpscaleQuality::kBalanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
    case UpscaleQuality::kPerformance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case UpscaleQuality::kUltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
  }
  return NVSDK_NGX_PerfQuality_Value_MaxQuality;
}

const char* ModeName(UpscaleQuality q) {
  switch (q) {
    case UpscaleQuality::kNative: return "DLAA";
    case UpscaleQuality::kQuality: return "Quality";
    case UpscaleQuality::kBalanced: return "Balanced";
    case UpscaleQuality::kPerformance: return "Performance";
    case UpscaleQuality::kUltraPerformance: return "Ultra Performance";
  }
  return "";
}

class DlssUpscaler final : public Upscaler {
 public:
  DlssUpscaler(nvrhi::IDevice* device, const UpscaleConfig& config, NVSDK_NGX_Parameter* caps)
      : device_(device), config_(config), caps_(caps), version_(DllVersion()) {
    vulkan_ = device->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
  }
  ~DlssUpscaler() override {
    Release();
    ReleaseNgx();
  }

  std::string Name() const override {
    std::string n = "DLSS";
    if (!version_.empty()) n += " " + version_;
    n += std::string(" ") + ModeName(config_.quality);
    if (const char preset = DlssPresetFor(config_.dlss_preset, config_.quality)) n += std::string(" preset ") + preset;
    n += vulkan_ ? " (Vulkan)" : " (D3D12)";
    return n;
  }

  UpscalerSize InputSize(UpscalerSize output) const override {
    unsigned w = 0, h = 0, max_w = 0, max_h = 0, min_w = 0, min_h = 0;
    float sharpness = 0.0f;
    const NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(caps_, output.width, output.height,
                                                            PerfQuality(config_.quality), &w, &h, &max_w, &max_h,
                                                            &min_w, &min_h, &sharpness);
    if (NVSDK_NGX_SUCCEED(r) && w > 0 && h > 0) return {w, h};
    const float ratio = DefaultUpscaleRatio(config_.quality);
    return {UpscaleInputSize(output.width, ratio), UpscaleInputSize(output.height, ratio)};
  }

  bool Evaluate(nvrhi::ICommandList* cl, const UpscalerInputs& in) override {
    if (!in.color || !in.depth || !in.motion || !in.output) return false;
    cl->clearState();
    cl->setTextureState(in.color, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    cl->setTextureState(in.depth, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    cl->setTextureState(in.motion, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    if (in.reactive) cl->setTextureState(in.reactive, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    cl->setTextureState(in.output, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    cl->commitBarriers();
#if NR_HAS_VULKAN
      if (vulkan_) SynchronizeVulkanSdk(cl);
#endif
    bool ok = true;
    if (!feature_ || in.render.width != render_.width || in.render.height != render_.height ||
        in.output_size.width != output_.width || in.output_size.height != output_.height) {
      ok = Create(cl, in.render, in.output_size);
    }
    if (ok) ok = vulkan_ ? EvaluateVulkan(cl, in) : EvaluateD3D12(cl, in);
#if NR_HAS_VULKAN
      if (vulkan_) SynchronizeVulkanSdk(cl);
#endif
    cl->clearState();
    return ok;
  }

  uint64_t GpuMemory() const override {
    unsigned long long bytes = 0;
    if (feature_ && NVSDK_NGX_SUCCEED(NGX_DLSS_GET_STATS(caps_, &bytes))) return bytes;
    return 0;
  }

 private:
#if NR_HAS_VULKAN
    void SynchronizeVulkanSdk(nvrhi::ICommandList* cl) const {
      // NGX can clear GENERAL-layout images with transfer commands. NVRHI's
      // UnorderedAccess state covers shader writes only, so synchronize all SDK
      // accesses at entry and exit, including clears and layout transitions.
      auto device = static_cast<VkDevice>(device_->getNativeObject(nvrhi::ObjectTypes::VK_Device).pointer);
      auto cb = static_cast<VkCommandBuffer>(cl->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer).pointer);
      auto barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(VulkanGetDeviceProcAddr()(device, "vkCmdPipelineBarrier"));
      VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      memory.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      memory.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
              0, 1, &memory, 0, nullptr, 0, nullptr);
    }
#endif
  void Release() {
    if (!feature_) return;
    device_->waitForIdle();
#if NR_HAS_VULKAN
    if (vulkan_) NVSDK_NGX_VULKAN_ReleaseFeature(feature_);
#endif
    if (!vulkan_) NVSDK_NGX_D3D12_ReleaseFeature(feature_);
    feature_ = nullptr;
  }

  bool Create(nvrhi::ICommandList* cl, UpscalerSize render, UpscalerSize output) {
    Release();
    if (const char letter = DlssPresetFor(config_.dlss_preset, config_.quality)) {
      // The letter's enum value: A is 1, J 10, K 11, L 12, M 13.
      const unsigned preset = unsigned(letter - 'A' + 1);
      for (const char* key : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality}) {
        NVSDK_NGX_Parameter_SetUI(caps_, key, preset);
      }
    }
    NVSDK_NGX_DLSS_Create_Params p{};
    p.Feature.InWidth = render.width;
    p.Feature.InHeight = render.height;
    p.Feature.InTargetWidth = output.width;
    p.Feature.InTargetHeight = output.height;
    p.Feature.InPerfQualityValue = PerfQuality(config_.quality);
    // Motion vectors at the input size and without the jitter; depth 0 near; the colour LDR.
    p.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Fail;
#if NR_HAS_VULKAN
    if (vulkan_) {
      auto vk_device = static_cast<VkDevice>(device_->getNativeObject(nvrhi::ObjectTypes::VK_Device).pointer);
      auto cb = static_cast<VkCommandBuffer>(cl->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer).pointer);
      r = NGX_VULKAN_CREATE_DLSS_EXT1(vk_device, cb, 1, 1, &feature_, caps_, &p);
    }
#endif
    if (!vulkan_) {
      auto* d3d_cl = static_cast<ID3D12GraphicsCommandList*>(cl->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList).pointer);
      r = NGX_D3D12_CREATE_DLSS_EXT(d3d_cl, 1, 1, &feature_, caps_, &p);
    }
    if (NVSDK_NGX_FAILED(r) || !feature_) {
      Logf(LogLevel::kWarning, "rexgpu-native: DLSS feature not created (%s) for %ux%u to %ux%u", ResultText(r).c_str(),
           render.width, render.height, output.width, output.height);
      feature_ = nullptr;
      return false;
    }
    render_ = render;
    output_ = output;
    Logf(LogLevel::kInfo, "rexgpu-native: %s for %ux%u to %ux%u", Name().c_str(), render.width, render.height,
         output.width, output.height);
    return true;
  }

  float FrameDelta(const UpscalerInputs& in) {
    const auto now = std::chrono::steady_clock::now();
    float dt = in.frame_ms;
    if (last_.time_since_epoch().count()) dt = std::chrono::duration<float, std::milli>(now - last_).count();
    last_ = now;
    return dt > 0.0f && dt < 200.0f ? dt : 33.3f;
  }

#if NR_HAS_VULKAN
  NVSDK_NGX_Resource_VK VkResource(nvrhi::ITexture* t, bool read_write) const {
    const nvrhi::TextureDesc& d = t->getDesc();
    auto view = static_cast<VkImageView>(
        t->getNativeView(nvrhi::ObjectTypes::VK_ImageView, d.format, nvrhi::AllSubresources, nvrhi::TextureDimension::Texture2D)
            .pointer);
    auto image = static_cast<VkImage>(t->getNativeObject(nvrhi::ObjectTypes::VK_Image).pointer);
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return NVSDK_NGX_Create_ImageView_Resource_VK(view, image, range, static_cast<VkFormat>(nvrhi::vulkan::convertFormat(d.format)),
                                                  d.width, d.height, read_write);
  }
#endif

  bool EvaluateVulkan(nvrhi::ICommandList* cl, const UpscalerInputs& in) {
#if NR_HAS_VULKAN
    NVSDK_NGX_Resource_VK color = VkResource(in.color, false), output = VkResource(in.output, true),
                          depth = VkResource(in.depth, false), motion = VkResource(in.motion, false), reactive{};
    if (in.reactive) reactive = VkResource(in.reactive, false);
    NVSDK_NGX_VK_DLSS_Eval_Params e{};
    e.Feature.pInColor = &color;
    e.Feature.pInOutput = &output;
    e.pInDepth = &depth;
    e.pInMotionVectors = &motion;
    e.pInBiasCurrentColorMask = in.reactive ? &reactive : nullptr;
    // The picture's shift in input pixels (+x right, +y down) and last frame's position minus this one's.
    e.InJitterOffsetX = in.jitter_x;
    e.InJitterOffsetY = in.jitter_y;
    e.InMVScaleX = 1.0f;
    e.InMVScaleY = 1.0f;
    e.InRenderSubrectDimensions = {in.render.width, in.render.height};
    e.InReset = in.reset ? 1 : 0;
    e.InPreExposure = 1.0f;
    e.InExposureScale = 1.0f;
    e.InFrameTimeDeltaInMsec = FrameDelta(in);
    auto cb = static_cast<VkCommandBuffer>(cl->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer).pointer);
    const NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(cb, feature_, caps_, &e);
    if (NVSDK_NGX_FAILED(r)) {
      if (failures_++ < 10) Logf(LogLevel::kWarning, "rexgpu-native: DLSS evaluation failed (%s)", ResultText(r).c_str());
      return false;
    }
    return true;
#else
    (void)cl;
    (void)in;
    return false;
#endif
  }

  bool EvaluateD3D12(nvrhi::ICommandList* cl, const UpscalerInputs& in) {
    auto res = [](nvrhi::ITexture* t) {
      return t ? static_cast<ID3D12Resource*>(t->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource).pointer) : nullptr;
    };
    NVSDK_NGX_D3D12_DLSS_Eval_Params e{};
    e.Feature.pInColor = res(in.color);
    e.Feature.pInOutput = res(in.output);
    e.pInDepth = res(in.depth);
    e.pInMotionVectors = res(in.motion);
    e.pInBiasCurrentColorMask = res(in.reactive);
    e.InJitterOffsetX = in.jitter_x;
    e.InJitterOffsetY = in.jitter_y;
    e.InMVScaleX = 1.0f;
    e.InMVScaleY = 1.0f;
    e.InRenderSubrectDimensions = {in.render.width, in.render.height};
    e.InReset = in.reset ? 1 : 0;
    e.InPreExposure = 1.0f;
    e.InExposureScale = 1.0f;
    e.InFrameTimeDeltaInMsec = FrameDelta(in);
    auto* d3d_cl = static_cast<ID3D12GraphicsCommandList*>(cl->getNativeObject(nvrhi::ObjectTypes::D3D12_GraphicsCommandList).pointer);
    const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(d3d_cl, feature_, caps_, &e);
    if (NVSDK_NGX_FAILED(r)) {
      if (failures_++ < 10) Logf(LogLevel::kWarning, "rexgpu-native: DLSS evaluation failed (%s)", ResultText(r).c_str());
      return false;
    }
    return true;
  }

  nvrhi::IDevice* device_;
  UpscaleConfig config_;
  NVSDK_NGX_Parameter* caps_;
  std::string version_;
  bool vulkan_ = true;
  NVSDK_NGX_Handle* feature_ = nullptr;
  UpscalerSize render_, output_;
  std::chrono::steady_clock::time_point last_{};
  uint32_t failures_ = 0;
};

}  // namespace

std::unique_ptr<Upscaler> CreateDlssUpscaler(nvrhi::IDevice* device, const UpscaleConfig& config, std::string* why) {
  if (UpscalerDeviceVendor(device) != 0x10DE) {
    if (why) *why = "not an NVIDIA GPU";
    return nullptr;
  }
  if (!DllPresent()) {
    if (why) *why = "nvngx_dlss.dll is not beside the game";
    return nullptr;
  }
  g_ngx_log = (config.debug & 16) != 0;
  NVSDK_NGX_Parameter* caps = AcquireNgx(device, why);
  if (!caps) return nullptr;
  return std::make_unique<DlssUpscaler>(device, config, caps);
}

#if NR_HAS_VULKAN
std::vector<std::string> DlssVulkanInstanceExtensions() {
  std::vector<std::string> out;
  if (!DllPresent()) return out;
  const NVSDK_NGX_FeatureDiscoveryInfo d = Discovery();
  uint32_t count = 0;
  VkExtensionProperties* props = nullptr;
  if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&d, &count, &props)) && props) {
    for (uint32_t i = 0; i < count; ++i) out.emplace_back(props[i].extensionName);
  }
  return out;
}

UpscalerVulkanNeeds DlssVulkanDeviceNeeds(VkInstance instance, VkPhysicalDevice physical) {
  UpscalerVulkanNeeds out;
  if (!DllPresent()) return out;
  const NVSDK_NGX_FeatureDiscoveryInfo d = Discovery();
  uint32_t count = 0;
  VkExtensionProperties* props = nullptr;
  if (NVSDK_NGX_SUCCEED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, physical, &d, &count, &props)) &&
      props) {
    for (uint32_t i = 0; i < count; ++i) out.extensions.emplace_back(props[i].extensionName);
  }
  // NGX takes buffer device addresses when it asks for the extension (core in Vulkan 1.2).
  for (const std::string& e : out.extensions) {
    if (e == VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) {
      VkPhysicalDeviceVulkan12Features f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
      f.bufferDeviceAddress = VK_TRUE;
      const auto* b = reinterpret_cast<const uint8_t*>(&f);
      out.features.emplace_back(b, b + sizeof(f));
    }
  }
  return out;
}
#endif

}  // namespace nr

#else  // !NR_HAS_DLSS

namespace nr {
std::unique_ptr<Upscaler> CreateDlssUpscaler(nvrhi::IDevice*, const UpscaleConfig&, std::string* why) {
  if (why) *why = "not built in (the DLSS SDK was not found at build time: NR_DLSS_SDK_DIR)";
  return nullptr;
}
#if NR_HAS_VULKAN
std::vector<std::string> DlssVulkanInstanceExtensions() { return {}; }
UpscalerVulkanNeeds DlssVulkanDeviceNeeds(VkInstance, VkPhysicalDevice) { return {}; }
#endif
}  // namespace nr

#endif
