// SMAA pixels and timestamp costs on both native APIs, without a window or game data.
#include "backend/smaa.h"
#include "backend/shaders.h"
#include "backend/tests/vk_test_device.h"
#include "backend/tests/memory_d3d12_test_device.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
using namespace nr;
using namespace nr::test;
static void Check(bool ok, const char* what) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
static nvrhi::TextureHandle Image(nvrhi::IDevice* d, unsigned w, unsigned h) {
  nvrhi::TextureDesc t; t.width = w; t.height = h; t.format = nvrhi::Format::RGBA8_UNORM;
  t.isRenderTarget = true; t.initialState = nvrhi::ResourceStates::ShaderResource; t.keepInitialState = true;
  return d->createTexture(t);
}
// Existing port FXAA shader, with exactly the Present constants (12 search steps).
class Fxaa {
  nvrhi::IDevice* d_;
  nvrhi::BindingLayoutHandle layout_;
  nvrhi::ShaderHandle vs_, ps_;
  nvrhi::TextureHandle gamma_;
 public:
  explicit Fxaa(nvrhi::IDevice* d) : d_(d) {
    auto shader = [&](ShaderId id, nvrhi::ShaderType type, const char* entry) {
      auto blob = GetShaderBytecode(d->getGraphicsAPI(), id);
      nvrhi::ShaderDesc s; s.shaderType = type; s.entryName = entry;
      return d->createShader(s, blob.data, blob.size);
    };
    vs_ = shader(ShaderId::kBlitVs, nvrhi::ShaderType::Vertex, "vs_main");
    ps_ = shader(ShaderId::kBlitPs, nvrhi::ShaderType::Pixel, "ps_main");
    nvrhi::BindingLayoutDesc l; l.visibility = nvrhi::ShaderType::All;
    l.bindings = {nvrhi::BindingLayoutItem::PushConstants(0, 48), nvrhi::BindingLayoutItem::Texture_SRV(0),
                  nvrhi::BindingLayoutItem::Texture_SRV(1)};
    layout_ = d->createBindingLayout(l); gamma_ = Image(d, 256, 1);
  }
  void Render(nvrhi::ICommandList* cl, nvrhi::ITexture* source, nvrhi::ITexture* target) {
    auto fb = d_->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));
    nvrhi::GraphicsPipelineDesc p; p.VS = vs_; p.PS = ps_; p.bindingLayouts = {layout_};
    p.primType = nvrhi::PrimitiveType::TriangleList;
    p.renderState.depthStencilState.depthTestEnable = p.renderState.depthStencilState.depthWriteEnable = false;
    p.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    auto pipeline = d_->createGraphicsPipeline(p, fb->getFramebufferInfo());
    using I = nvrhi::BindingSetItem;
    nvrhi::BindingSetDesc b; b.bindings = {I::PushConstants(0, 48), I::Texture_SRV(0, source), I::Texture_SRV(1, gamma_)};
    auto set = d_->createBindingSet(b, layout_);
    nvrhi::GraphicsState s; s.pipeline = pipeline; s.framebuffer = fb; s.bindings = {set};
    const auto& t = target->getDesc();
    s.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(t.width), float(t.height)));
    cl->setGraphicsState(s);
    const uint32_t c[12] = {0, 0, t.width, t.height, 0, 1, 2, 3, 16, 12, 0, 0};
    cl->setPushConstants(c, sizeof(c)); nvrhi::DrawArguments draw; draw.vertexCount = 3; cl->draw(draw);
  }
};

template<class Host> int Run(Host& host, const char* input, const char* output) {
  Check(bool(host), "device");
  auto* d = host->device(); Smaa aa(d); Check(aa.Initialize(), "embedded shaders / prewarm"); Fxaa fxaa(d);
  auto cl = d->createCommandList();
  auto submit = [&] { cl->close(); host->Execute(cl); host->WaitIdle(); };
  for (auto size : {std::pair(96u, 64u), std::pair(127u, 79u), std::pair(96u, 64u)}) {
    const auto [w, h] = size;
    auto source = Image(d, w, h), target = Image(d, w, h);
    std::vector<uint8_t> bytes(w * h * 4, 255);
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x)
      for (unsigned c = 0; c < 3; ++c) bytes[(y*w+x)*4+c] = x > y*3/2 ? 235 : 20;
    cl->open(); cl->writeTexture(source, 0, 0, bytes.data(), w*4);
    Check(aa.Render(cl, source, target), "diagonal render"); submit();
    auto p = host->ReadBack(target); unsigned smooth = 0;
    for (unsigned y = 3; y < h-3; ++y) for (unsigned x = 3; x < w-3; ++x) {
      const auto* q = p.at(x,y); Check(std::abs(q[3]-1.f) < 0.005f, "opaque alpha");
      if (q[0] > 0.1f && q[0] < 0.9f) ++smooth;
      if (std::abs(int(x)-int(y*3/2)) > 5)
        Check(std::abs(q[0]-float(bytes[(y*w+x)*4])/255.f) < 0.005f, "flat region is unchanged");
    }
    Check(smooth > 15, "diagonal edges have subpixel coverage");
    // A second, completely flat frame detects stale edges/weights and temporal history.
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
      auto* b = &bytes[(y*w+x)*4]; b[0] = 37; b[1] = 91; b[2] = 173; b[3] = 113;
    }
    cl->open(); cl->writeTexture(source, 0, 0, bytes.data(), w*4);
    Check(aa.Render(cl, source, target), "flat render after diagonal"); submit();
    p = host->ReadBack(target);
    for (size_t i = 0; i < bytes.size(); ++i)
      Check(std::abs(p.rgba[i]-float(bytes[i])/255.f) < 0.005f, "flat RGBA is preserved, no stale edges");
    cl->open(); Check(!aa.Render(cl, source, source), "in-place input rejected"); submit();
  }
  // Optional exact same-frame comparison and cost on a captured game image (PPM input).
  // Otherwise benchmark a deterministic synthetic staircase at both requested resolutions.
  for (unsigned h : {1440u, 2160u}) {
    unsigned w = h*16/9;
    std::vector<uint8_t> bytes(w*h*4,255);
    if (input && *input) {
      std::ifstream f(input, std::ios::binary); std::string magic; unsigned maxval;
      f >> magic >> w >> h >> maxval; f.get();
      Check(bool(f) && magic == "P6" && maxval == 255, "PPM image");
      std::vector<uint8_t> rgb(w*h*3); f.read(reinterpret_cast<char*>(rgb.data()), rgb.size());
      Check(size_t(f.gcount()) == rgb.size(), "complete PPM image"); bytes.assign(w*h*4,255);
      for (size_t i=0; i<rgb.size()/3; ++i) for (unsigned c=0;c<3;++c) bytes[i*4+c]=rgb[i*3+c];
    } else {
      for (unsigned y=0;y<h;++y) for (unsigned x=0;x<w;++x) for (unsigned c=0;c<3;++c)
        bytes[(y*w+x)*4+c]=((x+y/3)/37)%2 ? 235 : 20;
    }
    auto source=Image(d,w,h), target=Image(d,w,h);
    cl->open(); cl->writeTexture(source,0,0,bytes.data(),w*4);
    Check(aa.Render(cl,source,target), "benchmark upload/warmup"); submit();
    for (unsigned mode=0;mode<3;++mode) {
      std::vector<double> times;
      auto query=d->createTimerQuery();
      for (unsigned frame=0;frame<32;++frame) {
        cl->open(); cl->beginTimerQuery(query);
        if (mode==2) Check(aa.Render(cl,source,target), "SMAA benchmark");
        else if (mode==1) fxaa.Render(cl,source,target);
        else cl->copyTexture(target,nvrhi::TextureSlice(),source,nvrhi::TextureSlice());
        cl->endTimerQuery(query); submit();
        Check(d->pollTimerQuery(query), "timestamp complete");
        const double ms=1000.0*d->getTimerQueryTime(query); d->resetTimerQuery(query);
        if (frame>=8) times.push_back(ms);
      }
      std::sort(times.begin(),times.end());
      const char* name=mode==2?"smaa":mode==1?"fxaa":"off";
      std::printf("COST %s %ux%u %s median_ms=%.6f samples=%zu\n",host->device_name().c_str(),w,h,name,times[times.size()/2],times.size());
      if (output && *output) Check(WritePpm(std::string(output)+"_"+name+".ppm",host->ReadBack(target)), "write comparison");
    }
    if (input && *input) break;
  }
  Check(host->errors()==0, "API / NVRHI validation");
  std::puts("SMAA pixel, resize, clear and cost checks passed"); return 0;
}
int main(int argc, char** argv) {
  bool dx=false; const char* input=nullptr; const char* output=nullptr;
  for (int i=1;i<argc;++i) {
    if (std::string(argv[i])=="--d3d12") dx=true;
    else if (std::string(argv[i])=="--image" && i+1<argc) input=argv[++i];
    else if (std::string(argv[i])=="--out" && i+1<argc) output=argv[++i];
    else return 2;
  }
#if NR_HAS_D3D12
  if (dx) { auto d=MemoryD3D12TestDevice::Create(); return Run(d,input,output); }
#endif
  auto d=VulkanTestDevice::Create(true); return Run(d,input,output);
}
