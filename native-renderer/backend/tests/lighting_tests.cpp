// Synthetic GPU tests for the actual lighting passes, without a game process.
#include "backend/tests/check.h"
#include "backend/tests/vk_test_device.h"
#include "backend/scene_lighting.h"
#include "kkshaders/lighting.h"
#include "lighting_trace_vs.h"
#include "lighting_trace_ps.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace nr;
using namespace nr::test;
namespace {
nvrhi::TextureHandle Image(nvrhi::IDevice* d, uint32_t w, uint32_t h, nvrhi::Format f, bool target = false) {
  nvrhi::TextureDesc td;
  td.width = w; td.height = h; td.format = f; td.isRenderTarget = target;
  td.initialState = nvrhi::ResourceStates::ShaderResource; td.keepInitialState = true;
  return d->createTexture(td);
}
void Upload(nvrhi::IDevice* d, nvrhi::ITexture* t, const void* data, size_t pitch) {
  auto cl = d->createCommandList(); cl->open(); cl->writeTexture(t, 0, 0, data, pitch);
  cl->close(); d->executeCommandList(cl); d->waitForIdle();
}
float Mean(const Pixels& p) {
  double sum = 0; for(size_t i=0;i<p.rgba.size();i+=4) sum += p.rgba[i];
  return float(sum / (p.width * p.height));
}
}
TEST(Lighting_ShaftsAddLightOnlyInFog) {
  auto gpu = VulkanTestDevice::Create(); CHECK(gpu != nullptr); if(!gpu) return;
  auto d = gpu->device();
  const uint32_t w=192,h=128;
  auto color=Image(d,w,h,nvrhi::Format::RGBA8_UNORM,true);
  auto depth=Image(d,w,h,nvrhi::Format::R32_FLOAT);
  std::vector<uint32_t> rgb(w*h,0xB3333333);
  std::vector<float> z(w*h,0.9975f); // 20 units, near=0.05
  for(uint32_t y=0;y<h/3;y++) for(uint32_t x=w/3;x<2*w/3;x++) {
    rgb[y*w+x]=0xB3FFFFFF; z[y*w+x]=1.0f;
  }
  Upload(d,depth,z.data(),w*4);
  LightingConfig config; config.volumetrics=Volumetrics::kHigh;
  SceneLighting lighting(d,config); CHECK(lighting.Initialize());
  SceneLighting::VolumetricInput in; in.color=color;in.depth=depth;in.px=in.py=1;
  in.fog.color[0]=in.fog.color[1]=in.fog.color[2]=0.3f;
  in.fog.params[2]=1;in.fog.params[3]=0.04f;
  in.fog.normalizing[0]=-20;in.fog.normalizing[1]=-20;in.fog.normalizing[2]=1;
  auto render=[&](float density, bool bright) {
    auto input=rgb; if(!bright) std::fill(input.begin(),input.end(),0xB3333333);
    Upload(d,color,input.data(),w*4); in.fog.params[2]=density;
    auto before=gpu->ReadBack(color);
    auto cl=d->createCommandList();cl->open(); bool applied=lighting.Volumetric(cl,in);
    cl->close();d->executeCommandList(cl);d->waitForIdle();
    CHECK(applied == (density>0));
    auto after=gpu->ReadBack(color);
    float maxGain=0;
    for(size_t i=0;i<after.rgba.size();i+=4) {
      for(int c=0;c<3;c++) {CHECK(after.rgba[i+c]>=before.rgba[i+c]); maxGain=std::max(maxGain,after.rgba[i+c]-before.rgba[i+c]);}
      CHECK_EQ(after.rgba[i+3],before.rgba[i+3]);
    }
    std::printf("shafts density %.2f bright %d: mean gain %.6f, max %.6f\n",density,int(bright),Mean(after)-Mean(before),maxGain);
    if(density==0 || !bright) CHECK_NEAR(maxGain,0,1e-6);
    return Mean(after)-Mean(before);
  };
  float dense=render(1,true),thin=render(0.25f,true);
  CHECK(dense>0.01f);CHECK(dense>thin*2);render(0,true);render(1,false);
  CHECK_EQ(gpu->errors(),0);
}
TEST(Lighting_WaterPlaneReflectsBrightObject) {
  auto gpu=VulkanTestDevice::Create();CHECK(gpu!=nullptr);if(!gpu)return;
  auto d=gpu->device();
  const char* width=std::getenv("NR_WATER_BENCH_WIDTH");
  const uint32_t w=width ? uint32_t(std::atoi(width)) : 192,h=width ? w*9/16 : 128;
  // The pre-water scene has a bright wall at z=6 above the waterline, empty elsewhere.
  auto depth=Image(d,w,h,nvrhi::Format::RGBA32_FLOAT);
  auto color=Image(d,w,h,nvrhi::Format::RGBA8_UNORM);
  auto target=Image(d,w,h,nvrhi::Format::RGBA8_UNORM,true);
  std::vector<float> zs(w*h*4,1000);
  std::vector<uint32_t> rgb(w*h,0xFF000000);
  for(uint32_t y=h/8;y<h/2;y++) for(uint32_t x=w/4;x<3*w/4;x++) {zs[(y*w+x)*4]=6;rgb[y*w+x]=0xFF4080FF;}
  Upload(d,depth,zs.data(),w*16);Upload(d,color,rgb.data(),w*4);
  nvrhi::ShaderDesc sd;sd.shaderType=nvrhi::ShaderType::Vertex;sd.entryName="vs_main";
  auto vs=d->createShader(sd,lighting_trace_vs,sizeof(lighting_trace_vs));
  sd.shaderType=nvrhi::ShaderType::Pixel;sd.entryName="ps_main";
  auto ps=d->createShader(sd,lighting_trace_ps,sizeof(lighting_trace_ps));
  using I=nvrhi::BindingLayoutItem;using S=nvrhi::BindingSetItem;
  nvrhi::BindingLayoutDesc ld;ld.visibility=nvrhi::ShaderType::All;
  ld.bindings={I::Texture_SRV(0),I::Texture_SRV(1),I::Sampler(0),I::VolatileConstantBuffer(5)};
  auto layout=d->createBindingLayout(ld);
  nvrhi::BufferDesc bd;bd.byteSize=sizeof(kkshaders::LightingConstants);bd.isConstantBuffer=true;bd.isVolatile=true;bd.maxVersions=16;
  auto cb=d->createBuffer(bd);
  nvrhi::SamplerDesc sampler;sampler.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
  auto sam=d->createSampler(sampler);
  nvrhi::BindingSetDesc bs;bs.bindings={S::Texture_SRV(0,depth),S::Texture_SRV(1,color),S::Sampler(0,sam),S::ConstantBuffer(5,cb)};
  auto set=d->createBindingSet(bs,layout);
  nvrhi::FramebufferDesc fd;fd.addColorAttachment(target);auto fb=d->createFramebuffer(fd);
  nvrhi::GraphicsPipelineDesc pd;pd.VS=vs;pd.PS=ps;pd.bindingLayouts={layout};
  pd.renderState.depthStencilState.depthTestEnable=false;pd.renderState.depthStencilState.depthWriteEnable=false;
  pd.renderState.rasterState.cullMode=nvrhi::RasterCullMode::None;
  auto pipe=d->createGraphicsPipeline(pd,fb->getFramebufferInfo());
  kkshaders::LightingConstants k={};k.ssrTextures[2]=1;k.ssrTextures[3]=1;
  k.ssrRect[2]=w;k.ssrRect[3]=h;k.ssrSampler[1]=1;
  k.ssrDepth[0]=0.05f;k.ssrDepth[1]=1;k.ssrDepth[2]=0.05f;k.ssrDepth[3]=1000;
  k.ssrScreen[0]=2;k.ssrScreen[1]=-2;k.ssrScreen[2]=-1;k.ssrScreen[3]=1;
  k.ssrParams[0]=1;k.ssrParams[1]=3;k.ssrParams[2]=0.03f;k.ssrParams[3]=64;
  k.ssrMisc[0]=0.08f;k.ssrMisc[3]=0.02f;
  auto render=[&](bool valid) {
    k.ssrTextures[3]=valid;
    const int frames=width ? 25 : 1;
    auto tq=d->createTimerQuery();double sum=0;
    for(int frame=0;frame<frames;frame++) {
      auto cl=d->createCommandList();cl->open();cl->writeBuffer(cb,&k,sizeof(k));
      cl->beginTimerQuery(tq);
      nvrhi::GraphicsState gs;gs.pipeline=pipe;gs.framebuffer=fb;gs.bindings={set};
      gs.viewport.addViewport(nvrhi::Viewport(float(w),float(h)));gs.viewport.addScissorRect(nvrhi::Rect(0,w,0,h));
      cl->setGraphicsState(gs);nvrhi::DrawArguments args;args.vertexCount=3;cl->draw(args);
      cl->endTimerQuery(tq);cl->close();d->executeCommandList(cl);d->waitForIdle();
      if(frame>=5)sum+=d->getTimerQueryTime(tq)*1000;
      d->resetTimerQuery(tq);d->runGarbageCollection();
    }
    if(width)std::printf("water trace bench %ux%u %s: %.3f ms\n",w,h,valid ? "on" : "off",sum/20);
    return gpu->ReadBack(target);
  };
  auto off=render(false),on=render(true);float maxGain=0;int changed=0;
  for(uint32_t y=h*3/5;y<h;y++)for(uint32_t x=0;x<w;x++) {
    float delta=on.at(x,y)[0]-off.at(x,y)[0];maxGain=std::max(maxGain,delta);if(delta>0.02f)changed++;
    for(int c=0;c<3;c++)CHECK(on.at(x,y)[c]>=off.at(x,y)[c]-1e-6f);
    CHECK_EQ(on.at(x,y)[3],off.at(x,y)[3]);
  }
  std::printf("water: peak red gain %.6f, %d visibly reflected pixels\n",maxGain,changed);
  CHECK(maxGain>0.05f);CHECK(changed>100);CHECK_EQ(gpu->errors(),0);
}

TEST(Lighting_ShaftCost) {
  if (!std::getenv("NR_LIGHTING_BENCH")) return;
  auto gpu=VulkanTestDevice::Create(false);CHECK(gpu!=nullptr);if(!gpu)return;
  auto d=gpu->device();
  for(uint32_t w : {2560u,3840u}) {
    uint32_t h=w*9/16;
    auto color=Image(d,w,h,nvrhi::Format::RGBA8_UNORM,true);
    auto depth=Image(d,w,h,nvrhi::Format::R32_FLOAT);
    std::vector<uint32_t> rgb(w*h,0xFF333333);std::vector<float> z(w*h,0.9975f);
    for(uint32_t y=0;y<h/3;y++)for(uint32_t x=w/3;x<2*w/3;x++){rgb[y*w+x]=0xFFFFFFFF;z[y*w+x]=1;}
    Upload(d,depth,z.data(),w*4);
    for(auto level : {Volumetrics::kLow,Volumetrics::kHigh}) {
      LightingConfig config;config.volumetrics=level;SceneLighting lighting(d,config);CHECK(lighting.Initialize());
      SceneLighting::VolumetricInput in;in.color=color;in.depth=depth;in.px=in.py=1;
      in.fog.params[2]=1;in.fog.params[3]=0.04f;in.fog.normalizing[0]=-20;in.fog.normalizing[1]=-20;in.fog.normalizing[2]=1;
      auto tq=d->createTimerQuery();double sum=0;
      for(int frame=0;frame<25;frame++) {
        auto cl=d->createCommandList();cl->open();cl->writeTexture(color,0,0,rgb.data(),w*4);
        cl->beginTimerQuery(tq);CHECK(lighting.Volumetric(cl,in));cl->endTimerQuery(tq);cl->close();
        d->executeCommandList(cl);d->waitForIdle();if(frame>=5)sum+=d->getTimerQueryTime(tq)*1000;
        d->resetTimerQuery(tq);d->runGarbageCollection();
      }
      std::printf("shafts bench %ux%u %s: %.3f ms\n",w,h,VolumetricsName(level),sum/20);
    }
  }
}
NR_TEST_MAIN()
