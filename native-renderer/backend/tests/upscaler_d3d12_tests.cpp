// The upscalers on Direct3D 12, without the game renderer (docs/upscalers.md, "Tests").
//
// nr_game_upscale_tests runs them through the game renderer on Vulkan; this checks the D3D12 side of
// each SDK on its own: a device of its own on the first hardware adapter, a still synthetic picture
// (a disc and slanted stripes) sampled once per input pixel at the jittered position, the upscaled
// result after some frames against the same picture supersampled at the output size, and the plain
// bilinear stretch of the unjittered input as the bar to beat. With the jitter's sign flipped the result
// must get worse (the convention the renderer gives every upscaler). FSR 4 asked for on a GPU that cannot
// run it must fall back to FSR 3.1 and say why.

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

D3D12Device* Device() {
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
  return dev.get();
}

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
std::vector<float> Reference() {
  std::vector<float> r(size_t(kOutW) * kOutH);
  const float s = float(kInW) / kOutW;
  for (uint32_t y = 0; y < kOutH; ++y) {
    for (uint32_t x = 0; x < kOutW; ++x) {
      float sum = 0;
      for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) sum += Scene((x + (i + 0.5f) / 4) * s, (y + (j + 0.5f) / 4) * s);
      }
      r[size_t(y) * kOutW + x] = sum / 16;
    }
  }
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

double MeanDifference(const std::vector<float>& ref, const Pixels& p) {
  double sum = 0;
  for (uint32_t y = 0; y < kOutH; ++y) {
    for (uint32_t x = 0; x < kOutW; ++x) sum += std::fabs(p.at(x, y)[1] - ref[size_t(y) * kOutW + x]);
  }
  return sum / (double(kOutW) * kOutH);
}

// The bilinear stretch of the unjittered input.
double BilinearDifference(const std::vector<float>& ref) {
  const std::vector<uint8_t> in = InputFrame(0, 0);
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

struct Run {
  bool ran = false;
  std::string name;
  UpscalerKind chosen = UpscalerKind::kOff;
  std::string why;
  double difference = 0;
  int failures = 0;
};

Run Upscale(UpscalerKind kind, float jitter_sign, int frames = 32) {
  Run run;
  D3D12Device* d = Device();
  if (!d) return run;
  nvrhi::IDevice* dev = d->nvrhi;
  UpscaleConfig config;
  config.kind = kind;
  config.quality = UpscaleQuality::kPerformance;
  std::unique_ptr<Upscaler> u = CreateUpscaler(dev, config, &run.chosen, &run.why);
  if (!u) return run;
  run.name = u->Name();

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
  nvrhi::TextureHandle color = texture("color", kInW, kInH, nvrhi::Format::RGBA8_UNORM, false);
  nvrhi::TextureHandle depth = texture("depth", kInW, kInH, nvrhi::Format::R32_FLOAT, false);
  nvrhi::TextureHandle motion = texture("motion", kInW, kInH, nvrhi::Format::RG16_FLOAT, false);
  nvrhi::TextureHandle reactive = texture("reactive", kInW, kInH, nvrhi::Format::R8_UNORM, false);
  nvrhi::TextureHandle output = texture("output", kOutW, kOutH, nvrhi::Format::RGBA8_UNORM, true);
  const std::vector<float> depth_values(size_t(kInW) * kInH, 0.5f);
  const std::vector<uint8_t> zeros(size_t(kInW) * kInH * 4, 0);

  const uint32_t phases = JitterPhaseCount(float(kOutW) / kInW, float(kOutH) / kInH);
  nvrhi::CommandListHandle cl = dev->createCommandList();
  for (int i = 0; i < frames; ++i) {
    const Jitter j = JitterForFrame(uint64_t(i), phases);
    const std::vector<uint8_t> frame = InputFrame(j.x, j.y);
    cl->open();
    cl->writeTexture(color, 0, 0, frame.data(), kInW * 4);
    cl->writeTexture(depth, 0, 0, depth_values.data(), kInW * 4);
    cl->writeTexture(motion, 0, 0, zeros.data(), kInW * 4);
    cl->writeTexture(reactive, 0, 0, zeros.data(), kInW);
    UpscalerInputs in;
    in.color = color;
    in.depth = depth;
    in.motion = motion;
    in.reactive = reactive;
    in.output = output;
    in.render = {kInW, kInH};
    in.output_size = {kOutW, kOutH};
    in.jitter_x = j.x * jitter_sign;
    in.jitter_y = j.y * jitter_sign;
    in.reset = i == 0;
    in.frame_ms = 16.7f;
    if (!u->Evaluate(cl, in)) ++run.failures;
    cl->close();
    dev->executeCommandList(cl);
    dev->waitForIdle();
    dev->runGarbageCollection();
  }
  const Pixels out = nr::test::ReadBackTexture(dev, output);
  static const std::vector<float> ref = Reference();
  run.difference = MeanDifference(ref, out);
  if (const char* dir = std::getenv("NR_TEST_IMAGES")) {
    nr::test::WritePpm(std::string(dir) + "/upscaler_d3d12_" + UpscalerKindName(kind) + ".ppm", out);
  }
  run.ran = true;
  u.reset();
  dev->waitForIdle();
  return run;
}

void CheckUpscaler(UpscalerKind kind) {
  if (!Device()) {
    std::printf("  no Direct3D 12 device: skipped\n");
    return;
  }
  const Run right = Upscale(kind, 1.0f);
  if (!right.ran || right.chosen != kind) {
    std::printf("  %s is not available here (%s): skipped\n", UpscalerKindName(kind), right.why.c_str());
    return;
  }
  const Run flipped = Upscale(kind, -1.0f);
  static const double bilinear = BilinearDifference(Reference());
  std::printf("  %s: mean difference to the supersampled picture %.4f (jitter flipped %.4f), bilinear stretch %.4f; "
              "failures %d\n",
              right.name.c_str(), right.difference, flipped.difference, bilinear, right.failures);
  CHECK_EQ(right.failures, 0);
  CHECK(right.difference < bilinear);
  CHECK(right.difference < flipped.difference);
  CHECK_EQ(g_messages.errors, 0);
}

}  // namespace

TEST(UpscalerD3D12_Fsr3) { CheckUpscaler(UpscalerKind::kFsr3); }
TEST(UpscalerD3D12_Dlss) { CheckUpscaler(UpscalerKind::kDlss); }
TEST(UpscalerD3D12_Xess) { CheckUpscaler(UpscalerKind::kXess); }
TEST(UpscalerD3D12_Fsr4) {
  // On an AMD Radeon RX 9000 or 7000 GPU FSR 4 runs; anywhere else it falls back to FSR 3.1 and says why.
  if (!Device()) return;
  UpscaleConfig config;
  config.kind = UpscalerKind::kFsr4;
  config.quality = UpscaleQuality::kPerformance;
  UpscalerKind chosen = UpscalerKind::kOff;
  std::string why;
  std::unique_ptr<Upscaler> u = CreateUpscaler(Device()->nvrhi, config, &chosen, &why);
  std::printf("  asked for FSR 4: %s (%s)\n", u ? u->Name().c_str() : "nothing", why.c_str());
  CHECK(u != nullptr);
  if (chosen == UpscalerKind::kFsr4) {
    u.reset();
    CheckUpscaler(UpscalerKind::kFsr4);
  } else {
    CHECK(chosen == UpscalerKind::kFsr3);
    CHECK(why.find("FSR 4") != std::string::npos);
  }
}
TEST(UpscalerD3D12_Support) {
  if (!Device()) return;
  const UpscalerSupport s = QueryUpscalerSupport(Device()->nvrhi);
  std::printf("  vendor 0x%04X: FSR 3.1 %d, FSR 4 %d, DLSS %d, XeSS %d (%s)\n", s.vendor, s.fsr3, s.fsr4, s.dlss, s.xess,
              s.notes.c_str());
  CHECK(s.fsr3);
}

NR_TEST_MAIN()
