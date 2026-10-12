// The upscalers' SDKs on their own, on Direct3D 12 and Vulkan, without the game renderer
// (docs/upscalers.md, "Tests").
//
// nr_game_upscale_tests runs them through the game renderer on Vulkan; this checks each SDK alone on
// both APIs: a device of its own on the first hardware adapter, a still synthetic picture (a disc and
// slanted stripes) sampled once per input pixel at the jittered position, the upscaled result after some
// frames against the same picture supersampled at the output size, and the plain bilinear stretch of the
// unjittered input as the bar to beat. With the jitter's sign flipped the result must get worse (the
// convention the renderer gives every upscaler). FSR 4 asked for on a GPU that cannot run it must fall
// back to FSR 3.1 and say why.
//
// With NR_UPSCALER_COST set, *_Cost also times each SDK's upscale (GPU timer queries) at the game's
// usual sizes: 2560x1440 out, in every mode.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <nvrhi/d3d12.h>

#include "backend/tests/check.h"
#include "backend/tests/readback.h"
#if NR_HAS_VULKAN
#include "backend/tests/vk_test_device.h"
#endif
#include "backend/upscale_config.h"
#include "backend/upscale_jitter.h"
#include "backend/upscaler.h"

using namespace nr;
using nr::test::Pixels;

namespace {

class Messages final : public nvrhi::IMessageCallback {
 public:
  void message(nvrhi::MessageSeverity severity, const char* text) override {
    if (severity == nvrhi::MessageSeverity::Error || severity == nvrhi::MessageSeverity::Fatal) {
      ++errors;
      std::fprintf(stderr, "  [nvrhi] %s\n", text);
    }
  }
  int errors = 0;
};
Messages g_messages;

struct D3D12Device {
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  nvrhi::DeviceHandle nvrhi;
  std::string name;
};

nvrhi::IDevice* D3D12() {
  static std::unique_ptr<D3D12Device> dev = []() -> std::unique_ptr<D3D12Device> {
    IDXGIFactory6* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory)))) return nullptr;
    auto d = std::make_unique<D3D12Device>();
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1),
                                                         reinterpret_cast<void**>(&adapter)) == S_OK;
         ++i) {
      DXGI_ADAPTER_DESC1 desc{};
      adapter->GetDesc1(&desc);
      if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
          SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device),
                                      reinterpret_cast<void**>(&d->device)))) {
        for (const wchar_t* p = desc.Description; *p; ++p) d->name += *p < 128 ? char(*p) : '?';
        adapter->Release();
        break;
      }
      adapter->Release();
    }
    factory->Release();
    if (!d->device) return nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(d->device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&d->queue)))) {
      return nullptr;
    }
    nvrhi::d3d12::DeviceDesc desc;
    desc.errorCB = &g_messages;
    desc.pDevice = d->device;
    desc.pGraphicsCommandQueue = d->queue;
    d->nvrhi = nvrhi::d3d12::createDevice(desc);
    if (!d->nvrhi) return nullptr;
    std::printf("  device: %s (Direct3D 12)\n", d->name.c_str());
    return d;
  }();
  return dev ? dev->nvrhi.Get() : nullptr;
}

nvrhi::IDevice* Vulkan() {
#if NR_HAS_VULKAN
  // Without the Khronos layer (the SDKs' own work is not ours to validate), with what DLSS and XeSS ask for.
  static std::unique_ptr<nr::test::VulkanTestDevice> dev = [] {
    auto d = nr::test::VulkanTestDevice::Create(false, false, /*upscaler_needs=*/true);
    if (d) std::printf("  device: %s (Vulkan)\n", d->device_name().c_str());
    return d;
  }();
  return dev ? dev->device() : nullptr;
#else
  return nullptr;
#endif
}

nvrhi::IDevice* Device(nvrhi::GraphicsAPI api) { return api == nvrhi::GraphicsAPI::D3D12 ? D3D12() : Vulkan(); }
const char* ApiName(nvrhi::GraphicsAPI api) { return api == nvrhi::GraphicsAPI::D3D12 ? "D3D12" : "Vulkan"; }

constexpr uint32_t kInW = 192, kInH = 108, kOutW = 384, kOutH = 216;

// The picture, in input pixels: a disc and slanted stripes (black and white).
float Scene(float x, float y) {
  const float dx = x - 70.0f, dy = y - 54.0f;
  if (dx * dx + dy * dy < 30.0f * 30.0f) return 1.0f;
  if (x > 110.0f) {
    const float t = (x * 0.9f + y * 0.4f) / 7.0f;
    return t - std::floor(t) < 0.5f ? 1.0f : 0.0f;
  }
  return 0.15f;
}

// The picture at the output size, 4x4 samples a pixel.
const std::vector<float>& Reference() {
  static const std::vector<float> r = [] {
    std::vector<float> v(size_t(kOutW) * kOutH);
    const float s = float(kInW) / kOutW;
    for (uint32_t y = 0; y < kOutH; ++y) {
      for (uint32_t x = 0; x < kOutW; ++x) {
        float sum = 0;
        for (int j = 0; j < 4; ++j) {
          for (int i = 0; i < 4; ++i) sum += Scene((x + (i + 0.5f) / 4) * s, (y + (j + 0.5f) / 4) * s);
        }
        v[size_t(y) * kOutW + x] = sum / 16;
      }
    }
    return v;
  }();
  return r;
}

// One input frame: each input pixel samples the picture once, at its centre moved by the jitter (the
// picture shifted by +jx right, +jy down).
std::vector<uint8_t> InputFrame(float jx, float jy) {
  std::vector<uint8_t> px(size_t(kInW) * kInH * 4);
  for (uint32_t y = 0; y < kInH; ++y) {
    for (uint32_t x = 0; x < kInW; ++x) {
      const uint8_t v = uint8_t(std::lround(Scene(x + 0.5f - jx, y + 0.5f - jy) * 255.0f));
      uint8_t* p = &px[(size_t(y) * kInW + x) * 4];
      p[0] = p[1] = p[2] = v;
      p[3] = 255;
    }
  }
  return px;
}

double MeanDifference(const Pixels& p) {
  const std::vector<float>& ref = Reference();
  double sum = 0;
  for (uint32_t y = 0; y < kOutH; ++y) {
    for (uint32_t x = 0; x < kOutW; ++x) sum += std::fabs(p.at(x, y)[1] - ref[size_t(y) * kOutW + x]);
  }
  return sum / (double(kOutW) * kOutH);
}

// The bilinear stretch of the unjittered input.
double BilinearDifference() {
  const std::vector<uint8_t> in = InputFrame(0, 0);
  const std::vector<float>& ref = Reference();
  auto at = [&](int x, int y) {
    x = std::clamp(x, 0, int(kInW) - 1);
    y = std::clamp(y, 0, int(kInH) - 1);
    return in[(size_t(y) * kInW + x) * 4] / 255.0f;
  };
  double sum = 0;
  for (uint32_t y = 0; y < kOutH; ++y) {
    for (uint32_t x = 0; x < kOutW; ++x) {
      const float fx = (x + 0.5f) * kInW / kOutW - 0.5f, fy = (y + 0.5f) * kInH / kOutH - 0.5f;
      const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
      const float tx = fx - x0, ty = fy - y0;
      const float v = (at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx) * (1 - ty) +
                      (at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx) * ty;
      sum += std::fabs(v - ref[size_t(y) * kOutW + x]);
    }
  }
  return sum / (double(kOutW) * kOutH);
}

struct Textures {
  nvrhi::TextureHandle color, depth, motion, reactive, output;
};

Textures MakeTextures(nvrhi::IDevice* dev, uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h) {
  auto texture = [&](const char* name, uint32_t w, uint32_t h, nvrhi::Format format, bool uav) {
    nvrhi::TextureDesc t;
    t.width = w;
    t.height = h;
    t.format = format;
    t.debugName = name;
    t.isUAV = uav;
    t.initialState = uav ? nvrhi::ResourceStates::UnorderedAccess : nvrhi::ResourceStates::ShaderResource;
    t.keepInitialState = true;
    return dev->createTexture(t);
  };
  Textures t;
  t.color = texture("color", in_w, in_h, nvrhi::Format::RGBA8_UNORM, false);
  t.depth = texture("depth", in_w, in_h, nvrhi::Format::R32_FLOAT, false);
  t.motion = texture("motion", in_w, in_h, nvrhi::Format::RG16_FLOAT, false);
  t.reactive = texture("reactive", in_w, in_h, nvrhi::Format::R8_UNORM, false);
  t.output = texture("output", out_w, out_h, nvrhi::Format::RGBA8_UNORM, true);
  return t;
}

UpscalerInputs Inputs(const Textures& t, uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h) {
  UpscalerInputs in;
  in.color = t.color;
  in.depth = t.depth;
  in.motion = t.motion;
  in.reactive = t.reactive;
  in.output = t.output;
  in.render = {in_w, in_h};
  in.output_size = {out_w, out_h};
  in.frame_ms = 16.7f;
  return in;
}

struct Run {
  bool ran = false;
  std::string name;
  UpscalerKind chosen = UpscalerKind::kOff;
  std::string why;
  double difference = 0;
  int failures = 0;
};

Run Upscale(nvrhi::IDevice* dev, UpscalerKind kind, float jitter_sign, int frames = 32) {
  Run run;
  UpscaleConfig config;
  config.kind = kind;
  config.quality = UpscaleQuality::kPerformance;
  std::unique_ptr<Upscaler> u = CreateUpscaler(dev, config, &run.chosen, &run.why);
  if (!u) return run;
  run.name = u->Name();
  const Textures t = MakeTextures(dev, kInW, kInH, kOutW, kOutH);
  const std::vector<float> depth_values(size_t(kInW) * kInH, 0.5f);
  const std::vector<uint8_t> zeros(size_t(kInW) * kInH * 4, 0);
  const uint32_t phases = JitterPhaseCount(float(kOutW) / kInW, float(kOutH) / kInH);
  nvrhi::CommandListHandle cl = dev->createCommandList();
  for (int i = 0; i < frames; ++i) {
    const Jitter j = JitterForFrame(uint64_t(i), phases);
    const std::vector<uint8_t> frame = InputFrame(j.x, j.y);
    cl->open();
    cl->writeTexture(t.color, 0, 0, frame.data(), kInW * 4);
    cl->writeTexture(t.depth, 0, 0, depth_values.data(), kInW * 4);
    cl->writeTexture(t.motion, 0, 0, zeros.data(), kInW * 4);
    cl->writeTexture(t.reactive, 0, 0, zeros.data(), kInW);
    UpscalerInputs in = Inputs(t, kInW, kInH, kOutW, kOutH);
    in.jitter_x = j.x * jitter_sign;
    in.jitter_y = j.y * jitter_sign;
    in.reset = i == 0;
    if (!u->Evaluate(cl, in)) ++run.failures;
    cl->close();
    dev->executeCommandList(cl);
    dev->waitForIdle();
    dev->runGarbageCollection();
  }
  const Pixels out = nr::test::ReadBackTexture(dev, t.output);
  run.difference = MeanDifference(out);
  if (const char* dir = std::getenv("NR_TEST_IMAGES")) {
    nr::test::WritePpm(std::string(dir) + "/upscaler_" + ApiName(dev->getGraphicsAPI()) + "_" + UpscalerKindName(kind) +
                           (jitter_sign < 0 ? "_flipped" : "") + ".ppm",
                       out);
  }
  run.ran = true;
  u.reset();
  dev->waitForIdle();
  return run;
}

void CheckUpscaler(nvrhi::GraphicsAPI api, UpscalerKind kind) {
  nvrhi::IDevice* dev = Device(api);
  if (!dev) {
    std::printf("  no %s device: skipped\n", ApiName(api));
    return;
  }
  const Run right = Upscale(dev, kind, 1.0f);
  if (!right.ran || right.chosen != kind) {
    std::printf("  %s is not available on %s here (%s): skipped\n", UpscalerKindName(kind), ApiName(api),
                right.why.c_str());
    return;
  }
  const Run flipped = Upscale(dev, kind, -1.0f);
  static const double bilinear = BilinearDifference();
  std::printf("  %s: mean difference to the supersampled picture %.4f (jitter flipped %.4f), bilinear stretch %.4f; "
              "failures %d\n",
              right.name.c_str(), right.difference, flipped.difference, bilinear, right.failures);
  CHECK_EQ(right.failures, 0);
  CHECK(right.difference < bilinear);
  CHECK(right.difference < flipped.difference);
  CHECK_EQ(g_messages.errors, 0);
}

void CheckFsr4(nvrhi::GraphicsAPI api) {
  // On an AMD Radeon RX 9000 or 7000 GPU on D3D12 FSR 4 runs; anywhere else it falls back to FSR 3.1 and
  // says why.
  nvrhi::IDevice* dev = Device(api);
  if (!dev) return;
  UpscaleConfig config;
  config.kind = UpscalerKind::kFsr4;
  config.quality = UpscaleQuality::kPerformance;
  UpscalerKind chosen = UpscalerKind::kOff;
  std::string why;
  std::unique_ptr<Upscaler> u = CreateUpscaler(dev, config, &chosen, &why);
  std::printf("  asked for FSR 4 on %s: %s (%s)\n", ApiName(api), u ? u->Name().c_str() : "nothing", why.c_str());
  CHECK(u != nullptr);
  if (chosen == UpscalerKind::kFsr4) {
    u.reset();
    CheckUpscaler(api, UpscalerKind::kFsr4);
  } else {
    CHECK(chosen == UpscalerKind::kFsr3);
    CHECK(why.find("FSR 4") != std::string::npos);
  }
}

void CheckSupport(nvrhi::GraphicsAPI api) {
  nvrhi::IDevice* dev = Device(api);
  if (!dev) return;
  const UpscalerSupport s = QueryUpscalerSupport(dev);
  std::printf("  %s, vendor 0x%04X: FSR 3.1 %d, FSR 4 %d, DLSS %d, XeSS %d (%s)\n", ApiName(api), s.vendor, s.fsr3, s.fsr4,
              s.dlss, s.xess, s.notes.c_str());
  CHECK(s.fsr3);
}

// The GPU time of each SDK's upscale to 2560x1440 in every mode (the median of 60 frames after 20).
void Cost(nvrhi::GraphicsAPI api) {
  if (!std::getenv("NR_UPSCALER_COST")) {
    std::printf("  NR_UPSCALER_COST not set: skipped\n");
    return;
  }
  nvrhi::IDevice* dev = Device(api);
  if (!dev) return;
  constexpr uint32_t kW = 2560, kH = 1440;
  for (UpscalerKind kind : {UpscalerKind::kFsr3, UpscalerKind::kFsr4, UpscalerKind::kDlss, UpscalerKind::kXess}) {
    for (UpscaleQuality q : {UpscaleQuality::kNative, UpscaleQuality::kQuality, UpscaleQuality::kBalanced,
                             UpscaleQuality::kPerformance, UpscaleQuality::kUltraPerformance}) {
      UpscaleConfig config;
      config.kind = kind;
      config.quality = q;
      UpscalerKind chosen = UpscalerKind::kOff;
      std::string why;
      std::unique_ptr<Upscaler> u = CreateUpscaler(dev, config, &chosen, &why);
      if (!u || chosen != kind) break;
      const UpscalerSize in_size = u->InputSize({kW, kH});
      const Textures t = MakeTextures(dev, in_size.width, in_size.height, kW, kH);
      std::vector<uint8_t> pixels(size_t(in_size.width) * in_size.height * 4);
      for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = uint8_t((i * 2654435761u) >> 24);
      std::vector<float> depth(size_t(in_size.width) * in_size.height, 0.5f);
      std::vector<uint8_t> zeros(size_t(in_size.width) * in_size.height * 4, 0);
      nvrhi::CommandListHandle cl = dev->createCommandList();
      cl->open();
      cl->writeTexture(t.color, 0, 0, pixels.data(), in_size.width * 4);
      cl->writeTexture(t.depth, 0, 0, depth.data(), in_size.width * 4);
      cl->writeTexture(t.motion, 0, 0, zeros.data(), in_size.width * 4);
      cl->writeTexture(t.reactive, 0, 0, zeros.data(), in_size.width);
      cl->close();
      dev->executeCommandList(cl);
      dev->waitForIdle();
      const uint32_t phases = JitterPhaseCount(float(kW) / in_size.width, float(kH) / in_size.height);
      std::vector<float> ms;
      int failures = 0;
      for (int i = 0; i < 80; ++i) {
        nvrhi::TimerQueryHandle timer = dev->createTimerQuery();
        const Jitter j = JitterForFrame(uint64_t(i), phases);
        UpscalerInputs in = Inputs(t, in_size.width, in_size.height, kW, kH);
        in.jitter_x = j.x;
        in.jitter_y = j.y;
        in.reset = i == 0;
        cl->open();
        cl->beginTimerQuery(timer);
        if (!u->Evaluate(cl, in)) ++failures;
        cl->endTimerQuery(timer);
        cl->close();
        dev->executeCommandList(cl);
        dev->waitForIdle();
        if (i >= 20) ms.push_back(dev->getTimerQueryTime(timer) * 1000.0f);
        dev->runGarbageCollection();
      }
      std::sort(ms.begin(), ms.end());
      std::printf("  cost %s %-36s %4ux%-4u to %ux%u: %.3f ms (median; %.3f to %.3f)%s\n", ApiName(api), u->Name().c_str(),
                  in_size.width, in_size.height, kW, kH, ms[ms.size() / 2], ms.front(), ms.back(),
                  failures ? " FAILURES" : "");
      CHECK_EQ(failures, 0);
      u.reset();
      dev->waitForIdle();
    }
  }
}

}  // namespace

TEST(UpscalerD3D12_Fsr3) { CheckUpscaler(nvrhi::GraphicsAPI::D3D12, UpscalerKind::kFsr3); }
TEST(UpscalerD3D12_Dlss) { CheckUpscaler(nvrhi::GraphicsAPI::D3D12, UpscalerKind::kDlss); }
TEST(UpscalerD3D12_Xess) { CheckUpscaler(nvrhi::GraphicsAPI::D3D12, UpscalerKind::kXess); }
TEST(UpscalerD3D12_Fsr4) { CheckFsr4(nvrhi::GraphicsAPI::D3D12); }
TEST(UpscalerD3D12_Support) { CheckSupport(nvrhi::GraphicsAPI::D3D12); }
TEST(UpscalerD3D12_Cost) { Cost(nvrhi::GraphicsAPI::D3D12); }
#if NR_HAS_VULKAN
TEST(UpscalerVulkan_Fsr3) { CheckUpscaler(nvrhi::GraphicsAPI::VULKAN, UpscalerKind::kFsr3); }
TEST(UpscalerVulkan_Dlss) { CheckUpscaler(nvrhi::GraphicsAPI::VULKAN, UpscalerKind::kDlss); }
TEST(UpscalerVulkan_Xess) { CheckUpscaler(nvrhi::GraphicsAPI::VULKAN, UpscalerKind::kXess); }
TEST(UpscalerVulkan_Fsr4) { CheckFsr4(nvrhi::GraphicsAPI::VULKAN); }
TEST(UpscalerVulkan_Support) { CheckSupport(nvrhi::GraphicsAPI::VULKAN); }
TEST(UpscalerVulkan_Cost) { Cost(nvrhi::GraphicsAPI::VULKAN); }
#endif

NR_TEST_MAIN()
