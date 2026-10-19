// Run the presenter's actual embedded shaders into FP16 with known SDR/UI
// inputs, on both APIs. No window, game, assets or saves are opened.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
#ifdef REX_UI_HDR_CURVE_317
  // Task 317: no expansion at 0 for any colour (task 127's chroma-gated shoulder is gone).
  Near(base.at(3,0)[0], 2.5f, "yellow at paper white without expansion");
#else
  Near(base.at(3,0)[0], 10.194687f, "inverse shoulder on yellow highlight");
#endif
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
#if defined(REX_UI_HDR_HIGHLIGHTS) && !defined(REX_UI_HDR_CURVE_317)
// Task 262: highlight expansion. Below the knee exactly task 127's neutral mapping; SDR white reaches peak.
void Highlights(Fixture& f) {
  const std::vector<uint8_t> game = {0,0,0,255, 255,255,255,255, 128,128,128,255,
      255,255,0,255, 255,0,0,255, 64,64,64,255, 250,200,120,255, 0,0,0,255};
  const std::vector<uint8_t> none(game.size());
  const auto old = f.Render(8, 1, game, none, MakeConstants(200,1000,1000));
  const auto off = f.Render(8, 1, game, none, MakeConstants(200,1000,1000,0,0.5));
  for (size_t i = 0; i < old.rgba.size(); ++i) Check(old.rgba[i] == off.rgba[i], "expansion 0 = task 127 output");
  const auto on = f.Render(8, 1, game, none, MakeConstants(200,1000,1000,1,0.5));
  Near(on.at(0,0)[0], 0, "black stays black");
  Near(on.at(1,0)[0], 12.5f, "SDR white reaches peak (1000 nits)");
  Near(on.at(2,0)[0], 0.53965125f, "sRGB 128 midtone unchanged");
  Near(on.at(5,0)[0], 0.12817365f, "sRGB 64 unchanged");
  Near(on.at(3,0)[0], on.at(3,0)[1], "yellow hue kept");
  Check(on.at(3,0)[0] > 2.5f && on.at(3,0)[0] <= 12.5f, "yellow expands within peak");
  Check(on.at(6,0)[0] > old.at(6,0)[0] && on.at(6,0)[0] < 12.5f, "warm highlight expands");
  const float ratio = on.at(6,0)[1] / on.at(6,0)[0], before = old.at(6,0)[1] / old.at(6,0)[0];
  Check(std::abs(ratio - before) < 0.02f, "warm highlight keeps its hue");
  const auto half = f.Render(8, 1, game, none, MakeConstants(200,1000,1000,0.5,0.5));
  Near(half.at(1,0)[0], 7.5f, "half expansion: white at 600 nits");
  std::vector<uint8_t> ramp(256*4), empty(ramp.size());
  for (int x=0; x<256; ++x) { for (int c=0;c<3;++c) ramp[4*x+c]=uint8_t(x); ramp[4*x+3]=255; }
  const auto gray = f.Render(256,1,ramp,empty,MakeConstants(200,1000,1000,1,0.5));
  const auto base = f.Render(256,1,ramp,empty,MakeConstants(200,1000,1000));
  for (int x=0; x<256; ++x) {
    Check(std::isfinite(gray.at(x,0)[0]) && gray.at(x,0)[0] <= 12.5f, "finite, within peak");
    if (x <= 186) Check(gray.at(x,0)[0] == base.at(x,0)[0], "below the knee exactly as task 127");
  }
  // Large bright areas stop at the display's full-frame brightness; a lone highlight still reaches peak.
  std::vector<uint8_t> field(64*64*4, 255), spot(64*64*4, 0);
  for (int i = 3; i < int(spot.size()); i += 4) spot[i] = 255;
  for (int c = 0; c < 4; ++c) spot[(32*64+32)*4+c] = 255;
  const auto flat = f.Render(64,64,field,std::vector<uint8_t>(field.size()),MakeConstants(200,1000,1000,1,0.5,0,400));
  Near(flat.at(32,32)[0], 5.0f, "white field held at full-frame 400 nits");
  const auto lone = f.Render(64,64,spot,std::vector<uint8_t>(spot.size()),MakeConstants(200,1000,1000,1,0.5,0,400));
  Near(lone.at(32,32)[0], 12.5f, "lone white highlight reaches peak");
  // Calibrated black: black is lifted to it, paper white and above are not.
  const auto lifted = f.Render(8, 1, game, none, MakeConstants(200,1000,1000,1,0.5,0.4,0));
  Near(lifted.at(0,0)[0], 0.4f/80, "black lifted to the display's 0.4 nit black");
  Near(lifted.at(1,0)[0], 12.5f, "white unaffected by the black lift");
  std::puts("PASS: highlight expansion (off identical, midtones kept, white to peak, hue kept, full frame, black)");
}
#endif
#ifdef REX_UI_HDR_CURVE_317
// Task 317: one monotonic curve. Whiter is never darker, below the knee exactly c * paper, smooth roll-off into
// the ceiling (cap 1000 nits; large areas toward full-frame or half the ceiling), thin white text at paper white.
std::vector<uint8_t> Row(const std::vector<std::array<uint8_t,3>>& px) {
  std::vector<uint8_t> v; for (auto& p : px) { v.insert(v.end(), p.begin(), p.end()); v.push_back(255); } return v;
}
float Lum(const Pixels& p, uint32_t x, uint32_t y) {
  auto c = p.at(x,y); return 0.2126f*c[0] + 0.7152f*c[1] + 0.0722f*c[2];
}
void Curve317(Fixture& f) {
  // Constants: cap, full-frame fallback (unknown or equal to peak, as on the owner's LG TV 0/4000/4000).
  const auto tv = MakeConstants(200, 4000, 4000, 0.5, 0.65, 0, 4000);
  Near(tv.peak, 12.5f, "4000-nit calibration capped at 1000 nits"); Near(tv.full_frame, 5.0f, "full-frame = 0.4 x ceiling");
  Near(MakeConstants(200, 4000, 4000, 0.5, 0.65, 0, 400).full_frame, 5.0f, "known full-frame used");
  Near(MakeConstants(200, 4000, 4000, 0.5, 0.65, 0, 0, 2000).peak, 25.0f, "cap raised to 2000");
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const auto bad = MakeConstants(nan, nan, nan, nan, nan, nan, nan, nan, nan);
  for (float v : {bad.paper, bad.peak, bad.highlights, bad.knee, bad.black, bad.full_frame, bad.text_guard})
    Check(std::isfinite(v), "NaN cvars give finite constants");
  Check(bad.paper <= bad.full_frame && bad.full_frame <= bad.peak, "paper <= full-frame <= peak");
  // Grayscale: identical to the paper-white line up to the knee (sRGB 210 at 0.65), monotonic (1 code of dither).
  std::vector<uint8_t> ramp(256*4), empty(ramp.size());
  for (int x=0; x<256; ++x) { for (int c=0;c<3;++c) ramp[4*x+c]=uint8_t(x); ramp[4*x+3]=255; }
  const auto flat = f.Render(256,1,ramp,empty,MakeConstants(200,1000,1000,0,0.65,0,0,1000,0));
  const auto on = f.Render(256,1,ramp,empty,MakeConstants(200,1000,1000,0.5,0.65,0,0,1000,0));
  for (int x=0; x<256; ++x) {
    Check(std::isfinite(on.at(x,0)[0]) && on.at(x,0)[0] >= 0 && on.at(x,0)[0] <= 12.5f, "finite, within peak");
    if (x <= 210) Check(on.at(x,0)[0] == flat.at(x,0)[0], "below the knee exactly the SDR picture at paper white");
    // 1 code of dither, and in a 1-pixel-high picture the large-area ceiling follows the pixel itself (flat top):
    // within 2% of the value two codes darker. Without dither the curve is strictly monotonic (offline model).
    if (x >= 2) Check(on.at(x,0)[0] >= on.at(x-2,0)[0] * 0.98f, "grayscale monotonic");
    if (x >= 1) Check(flat.at(x,0)[0] >= flat.at(x-1,0)[0], "grayscale monotonic without expansion");
  }
  Near(on.at(255,0)[0], 3.75f, "white area: half way from paper white to the large-area ceiling (300 nits)");
  // White -> tinted ramps (cyan halo, warm, yellow, blue): the whitest pixel is the brightest, never darker.
  for (auto tint : {std::array<int,3>{0,255,255}, {255,216,153}, {255,255,0}, {0,0,255}}) {
    std::vector<std::array<uint8_t,3>> px;
    for (int i=0; i<64; ++i) { std::array<uint8_t,3> p{};
      for (int c=0;c<3;++c) p[c] = uint8_t(std::lround(255 + (tint[c]-255) * (i/63.0))); px.push_back(p); }
    const auto r = f.Render(64,1,Row(px),std::vector<uint8_t>(64*4),MakeConstants(200,1000,1000,1,0.65,0,0,1000,0));
    for (int x=2; x<64; ++x) Check(Lum(r,x,0) <= Lum(r,x-2,0) * 1.005f + 1e-4f, "more tint is never brighter");
    for (int x=0; x<64; ++x) Check(Lum(r,x,0) <= Lum(r,0,0) + 1e-4f, "pure white is the brightest");
  }
  // Lone highlight vs large area; thin white text (dark both sides) vs a white edge (dark on one side).
  std::vector<uint8_t> spot(64*64*4, 0), field(64*64*4, 255), stroke(64*64*4, 0), edge(64*64*4, 0);
  for (int i=3; i<64*64*4; i+=4) spot[i] = stroke[i] = edge[i] = 255;
  for (int y=0; y<64; ++y) for (int x=0; x<64; ++x) for (int c=0; c<3; ++c) {
    if (x == 32 && y == 32) spot[(y*64+x)*4+c] = 255;
    if (x == 31 || x == 32) stroke[(y*64+x)*4+c] = 255;
    if (x >= 32) edge[(y*64+x)*4+c] = 255;
  }
  const std::vector<uint8_t> none(64*64*4);
  const auto big = f.Render(64,64,field,none,MakeConstants(200,1000,1000,1,0.65,0,400,1000,1));
  Near(big.at(32,32)[0], 5.0f, "white field held at full-frame 400 nits");
  const auto lone = f.Render(64,64,spot,none,MakeConstants(200,1000,1000,1,0.65,0,400,1000,0));
  Near(lone.at(32,32)[0], 12.5f, "lone white highlight reaches the ceiling");
  const auto text = f.Render(64,64,stroke,none,MakeConstants(200,1000,1000,1,0.65,0,0,1000,1));
  Near(text.at(32,32)[0], 2.5f, "thin white stroke with dark sides stays at paper white");
  const auto side = f.Render(64,64,edge,none,MakeConstants(200,1000,1000,1,0.65,0,0,1000,1));
  Check(side.at(33,32)[0] > 5.0f, "white next to a dark edge still expands");
  // Hue, saturated colours within the ceiling, calibrated black.
  const auto colours = Row({{250,200,120},{255,0,0},{0,0,255},{255,255,0}});
  const auto warm = f.Render(4,1,colours,std::vector<uint8_t>(16),MakeConstants(200,1000,1000,1,0.65,0,0,1000,0));
  const auto warm0 = f.Render(4,1,colours,std::vector<uint8_t>(16),MakeConstants(200,1000,1000));
  Check(std::abs(warm.at(0,0)[1]/warm.at(0,0)[0] - warm0.at(0,0)[1]/warm0.at(0,0)[0]) < 0.02f, "hue kept");
  Check(warm.at(0,0)[0] > warm0.at(0,0)[0], "warm highlight expands");
  for (uint32_t x=0; x<4; ++x) for (int c=0; c<3; ++c) Check(warm.at(x,0)[c] <= 12.5f + 1e-3f, "within ceiling");
  Near(warm.at(3,0)[0], warm.at(3,0)[1], "yellow hue kept");
  const auto lifted = f.Render(4,1,Row({{0,0,0},{255,255,255},{128,128,128},{0,0,0}}),std::vector<uint8_t>(16),
                               MakeConstants(200,1000,1000,1,0.65,0.4,0,1000,0));
  Near(lifted.at(0,0)[0], 0.4f/80, "black lifted to the display's 0.4 nit black");
  Check(lifted.at(1,0)[0] > 2.5f, "white unaffected by the black lift");
  std::puts("PASS: task 317 curve (cap, full-frame fallback, knee identity, monotonic, white brightest, text guard)");
}
#endif
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
#ifdef REX_UI_HDR_CURVE_317
    Curve317(f);
#elif defined(REX_UI_HDR_HIGHLIGHTS)
    Highlights(f);
#endif
    if (argc >= 3) {
      std::ifstream in(argv[1],std::ios::binary); uint32_t w=0,h=0;
      in.read(reinterpret_cast<char*>(&w),4); in.read(reinterpret_cast<char*>(&h),4);
      Check(w && h && w<=4096 && h<=4096,"input dimensions");
      std::vector<uint8_t> input(size_t(w)*h*4); in.read(reinterpret_cast<char*>(input.data()),input.size());
      Check(bool(in),"input SDR pixels");
      // Optional (task 262 build only): <highlights> <knee> [black nits] [full-frame nits].
      auto values = MakeConstants(200,1000,1000);
#ifdef REX_UI_HDR_HIGHLIGHTS
      // <highlights> <knee> [black nits] [full-frame nits]
      if (argc >= 5) values = MakeConstants(200, 1000, 1000, std::atof(argv[3]), std::atof(argv[4]),
                                            argc > 5 ? std::atof(argv[5]) : 0, argc > 6 ? std::atof(argv[6]) : 0);
#endif
#ifdef REX_UI_HDR_CURVE_317
      // Task 317: [peak cap nits] [text guard]; paper 200, peak 4000 (the owner's LG TV calibration).
      if (argc >= 5) values = MakeConstants(200, 4000, 4000, std::atof(argv[3]), std::atof(argv[4]),
                                            argc > 5 ? std::atof(argv[5]) : 0, argc > 6 ? std::atof(argv[6]) : 0,
                                            argc > 7 ? std::atof(argv[7]) : 1000, argc > 8 ? std::atof(argv[8]) : 1);
#endif
      auto result=f.Render(w,h,input,std::vector<uint8_t>(input.size()),values);
      std::ofstream out(std::string(argv[2])+(dx?"-d3d12.f32":"-vulkan.f32"),std::ios::binary);
      out.write(reinterpret_cast<char*>(&w),4); out.write(reinterpret_cast<char*>(&h),4);
      out.write(reinterpret_cast<const char*>(result.rgba.data()),result.rgba.size()*sizeof(float));
      Check(bool(out),"float HDR capture");
    }
  }
  return 0;
} catch (const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
