// Run the presenter's actual embedded shaders into FP16 with known SDR/UI
// inputs, on both APIs. No window, game, assets or saves are opened.
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>
#include <rex/ui/hdr.h>
#include <nvrhi/d3d12.h>
#include <nvrhi/validation.h>
#include <wrl/client.h>
#include "backend/tests/vk_test_device.h"

namespace shaders {
#include "ui/shaders/bytecode/hdr_d3d12_vs.h"
#include "ui/shaders/bytecode/hdr_d3d12_ps.h"
const uint32_t hdr_vulkan_vs[] = {
#include "ui/shaders/bytecode/hdr_vulkan_vs.inc"
};
const uint32_t hdr_vulkan_ps[] = {
#include "ui/shaders/bytecode/hdr_vulkan_ps.inc"
};
}
using namespace nr::test;
using namespace rex::ui::hdr;
void Check(bool b, const char* what) { if (!b) throw std::runtime_error(what); }
void Near(float a, float b, const char* what) {
  if (std::abs(a-b) > std::max(0.003f, std::abs(b)*0.0015f)) {
    std::fprintf(stderr, "%s: got %.7f expected %.7f\n", what, a, b);
    throw std::runtime_error(what);
  }
}
struct Messages : nvrhi::IMessageCallback {
  int errors = 0;
  void message(nvrhi::MessageSeverity s, const char* msg) override {
    if (s == nvrhi::MessageSeverity::Error || s == nvrhi::MessageSeverity::Fatal) {
      ++errors; std::fprintf(stderr, "%s\n", msg);
    }
  }
};
struct Fixture {
  Messages messages;
  Microsoft::WRL::ComPtr<ID3D12Device> native;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
  std::unique_ptr<VulkanTestDevice> vulkan;
  nvrhi::DeviceHandle device;
  bool dx;
  explicit Fixture(bool directx) : dx(directx) {
    if (dx) {
      Check(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&native))), "D3D12 device");
      D3D12_COMMAND_QUEUE_DESC q{};
      Check(SUCCEEDED(native->CreateCommandQueue(&q, IID_PPV_ARGS(&queue))), "D3D12 queue");
      nvrhi::d3d12::DeviceDesc d;
      d.pDevice = native.Get(); d.pGraphicsCommandQueue = queue.Get(); d.errorCB = &messages;
      device = nvrhi::validation::createValidationLayer(nvrhi::d3d12::createDevice(d));
    } else {
      vulkan = VulkanTestDevice::Create(true);
      Check(bool(vulkan), "Vulkan device"); device = vulkan->device();
    }
    Check(bool(device), "NVRHI device");
  }
  ~Fixture() { device->waitForIdle(); device = nullptr; }
  Pixels Render(uint32_t w, uint32_t h, const std::vector<uint8_t>& game,
                const std::vector<uint8_t>& ui, Constants values) {
    nvrhi::TextureDesc td;
    td.width = w; td.height = h; td.format = nvrhi::Format::RGBA8_UNORM;
    td.initialState = nvrhi::ResourceStates::ShaderResource; td.keepInitialState = true;
    auto scene = device->createTexture(td), overlay = device->createTexture(td);
    td.format = nvrhi::Format::RGBA16_FLOAT; td.isRenderTarget = true;
    auto output = device->createTexture(td);
    auto fb = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(output));
    nvrhi::BindingLayoutDesc ld;
    ld.visibility = nvrhi::ShaderType::Pixel;
    ld.bindingOffsets.setShaderResourceOffset(0);
    ld.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, sizeof(values)),
        nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_SRV(1)};
    auto layout = device->createBindingLayout(ld);
    nvrhi::BindingSetDesc bd;
    bd.bindings = {nvrhi::BindingSetItem::PushConstants(0, sizeof(values)),
        nvrhi::BindingSetItem::Texture_SRV(0, scene), nvrhi::BindingSetItem::Texture_SRV(1, overlay)};
    auto set = device->createBindingSet(bd, layout);
    nvrhi::ShaderDesc sd; sd.shaderType = nvrhi::ShaderType::Vertex; sd.entryName = "vsmain";
    auto vs = device->createShader(sd, dx ? static_cast<const void*>(shaders::hdr_d3d12_vs) : shaders::hdr_vulkan_vs,
        dx ? sizeof(shaders::hdr_d3d12_vs) : sizeof(shaders::hdr_vulkan_vs));
    sd.shaderType = nvrhi::ShaderType::Pixel; sd.entryName = "psmain";
    auto ps = device->createShader(sd, dx ? static_cast<const void*>(shaders::hdr_d3d12_ps) : shaders::hdr_vulkan_ps,
        dx ? sizeof(shaders::hdr_d3d12_ps) : sizeof(shaders::hdr_vulkan_ps));
    nvrhi::GraphicsPipelineDesc pd;
    pd.VS = vs; pd.PS = ps; pd.primType = nvrhi::PrimitiveType::TriangleList; pd.bindingLayouts = {layout};
    pd.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    pd.renderState.depthStencilState.depthTestEnable = pd.renderState.depthStencilState.depthWriteEnable = false;
    auto pipeline = device->createGraphicsPipeline(pd, fb);
    Check(bool(pipeline), "HDR graphics pipeline");
    auto cl = device->createCommandList(); cl->open();
    cl->writeTexture(scene, 0, 0, game.data(), w*4);
    cl->writeTexture(overlay, 0, 0, ui.data(), w*4);
    nvrhi::GraphicsState state; state.pipeline = pipeline; state.framebuffer = fb; state.bindings = {set};
    state.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(w), float(h)));
    cl->setGraphicsState(state); cl->setPushConstants(&values, sizeof(values));
    nvrhi::DrawArguments draw; draw.vertexCount = 3; cl->draw(draw); cl->close();
    if (vulkan) vulkan->Execute(cl); else device->executeCommandList(cl);
    auto result = ReadBackTexture(device, output);
    device->waitForIdle(); device->runGarbageCollection();
    Check(messages.errors == 0 && (!vulkan || !vulkan->errors()), "GPU validation");
    return result;
  }
};
void KnownInputs(Fixture& f) {
  const std::vector<uint8_t> game = {0,0,0,255, 255,255,255,255, 128,128,128,255,
      255,255,0,255, 255,0,0,255, 64,64,64,255, 255,255,255,255, 0,0,0,255};
  std::vector<uint8_t> ui(game.size());
  // Opaque white UI, transparent texels with stray RGB, half-alpha white UI.
  ui[4*3] = ui[4*3+1] = ui[4*3+2] = ui[4*3+3] = 255;
  ui[4*4] = ui[4*4+1] = ui[4*4+2] = 255;
  ui[4*7] = ui[4*7+1] = ui[4*7+2] = ui[4*7+3] = 128;
  auto result = f.Render(8, 1, game, ui, MakeConstants(200,1000,1000));
  Near(result.at(0,0)[0], 0, "black stays black");
  Near(result.at(1,0)[0], 2.5f, "200 nit neutral white = scRGB 2.5");
  Near(result.at(2,0)[0], 0.53965125f, "sRGB 128 decodes to 43.1721 nits");
  Near(result.at(3,0)[0], 2.5f, "opaque UI white stays at paper white");
  Near(result.at(4,0)[0], 2.5f, "alpha-zero UI does not contribute");
  Near(result.at(5,0)[0], 0.12817365f, "sRGB 64 fog stays at paper-white exposure");
  Near(result.at(6,0)[0], 2.5f, "white guest subtitle stays at paper white");
  Near(result.at(7,0)[0], 2.5f*128/255, "premultiplied half-alpha UI");
  auto base = f.Render(8,1,game,std::vector<uint8_t>(game.size()),MakeConstants(200,1000,1000));
  Near(base.at(3,0)[0], 10.194687f, "inverse shoulder on yellow highlight");
  Near(base.at(3,0)[1], base.at(3,0)[0], "hue kept");
  // Full grayscale ramp, peak==paper, malicious cvar inputs, display ceiling.
  std::vector<uint8_t> ramp(256*4), empty(ramp.size());
  for (int x=0; x<256; ++x) { for (int c=0;c<3;++c) ramp[4*x+c]=uint8_t(x); ramp[4*x+3]=255; }
  auto gray = f.Render(256,1,ramp,empty,MakeConstants(200,1000,1000));
  for (int x=1;x<256;++x) Check(gray.at(x,0)[0]>=gray.at(x-1,0)[0], "monotonic grayscale");
  const auto dim = MakeConstants(std::numeric_limits<double>::quiet_NaN(), -1, 90);
  Check(dim.paper <= dim.peak && dim.peak <= 90.f/80 && std::isfinite(dim.paper), "finite dim-panel clamp");
  const auto clamp = MakeConstants(500,10000,400);
  Near(clamp.paper, 5, "paper limited to display"); Near(clamp.peak,5,"peak limited to display");
  const auto low = MakeConstants(500,100,1000);
  Near(low.paper, 1.25f, "paper limited to requested peak"); Near(low.peak,1.25f,"requested peak kept");
  auto ceiling = f.Render(256,1,ramp,empty,clamp);
  for (float v: ceiling.rgba) Check(std::isfinite(v) && v>=0 && v<=5, "finite bounded output");
  std::puts("PASS: known scRGB/UI values, inverse shoulder, grayscale, finite/peak clamps");
}
int main(int argc, char** argv) try {
  Check(!Eligible(false,false) && !Eligible(true,false) && !Eligible(false,true) && Eligible(true,true), "HDR eligibility");
  struct Pair { int format, colorSpace; };
  const std::vector<Pair> pairs{{44,0},{97,0},{44,1000104002},{97,1000104002}};
  Check(FindScRgb(pairs,97,1000104002) == pairs.begin()+3, "requires both FP16 and linear scRGB");
  const std::vector<Pair> sdr{{44,0}};
  Check(FindScRgb(sdr,97,1000104002) == sdr.end(), "single concrete SDR format cannot select HDR");
  const std::vector<Pair> wrong{{97,0},{44,1000104002}};
  Check(FindScRgb(wrong,97,1000104002) == wrong.end(), "format and colorspace from different pairs rejected");
  auto display = Query(nullptr);
  std::printf("Windows HDR %s, reported peak %.0f nits\n", display.enabled ? "on" : "off", display.peak);
  for (bool dx : {false,true}) {
    Fixture f(dx); std::puts(dx ? "D3D12" : "Vulkan"); KnownInputs(f);
    if (argc == 3) {
      std::ifstream in(argv[1],std::ios::binary); uint32_t w=0,h=0;
      in.read(reinterpret_cast<char*>(&w),4); in.read(reinterpret_cast<char*>(&h),4);
      Check(w && h && w<=4096 && h<=4096,"input dimensions");
      std::vector<uint8_t> input(size_t(w)*h*4); in.read(reinterpret_cast<char*>(input.data()),input.size());
      Check(bool(in),"input SDR pixels");
      auto result=f.Render(w,h,input,std::vector<uint8_t>(input.size()),MakeConstants(200,1000,1000));
      std::ofstream out(std::string(argv[2])+(dx?"-d3d12.f32":"-vulkan.f32"),std::ios::binary);
      out.write(reinterpret_cast<char*>(&w),4); out.write(reinterpret_cast<char*>(&h),4);
      out.write(reinterpret_cast<const char*>(result.rgba.data()),result.rgba.size()*sizeof(float));
      Check(bool(out),"float HDR capture");
    }
  }
  return 0;
} catch (const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
