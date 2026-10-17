// Synthetic GPU tests for the actual lighting passes, without a game process.
#include "backend/tests/check.h"
#include "backend/tests/vk_test_device.h"
#include "backend/scene_lighting.h"
#include "kkshaders/lighting.h"
#include "lighting_trace_vs.h"
#include "lighting_trace_ps.h"
#include "lighting_filter_vs.h"
#include "lighting_filter_pcf.h"
#include "lighting_filter_pcss.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace nr;
using namespace nr::test;
namespace {
nvrhi::TextureHandle Image(nvrhi::IDevice* d, uint32_t w, uint32_t h, nvrhi::Format f, bool target = false, uint32_t mips = 1) {
  nvrhi::TextureDesc td;
  td.width = w; td.height = h; td.format = f; td.isRenderTarget = target; td.mipLevels = mips;
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
SceneLighting::ShadowQuad SunMap(nvrhi::ITexture* map) {
  SceneLighting::ShadowQuad q;
  q.shadow_map = map;
  q.light[0] = q.light[5] = 0.02f;
  q.light[10] = -10.01f; q.light[11] = 10.0f;
  q.light[14] = -20.0f; q.light[15] = 20.0f;
  return q;
}
}
TEST(Lighting_ShaftsRequireSunVisibilityAndFog) {
  auto gpu = VulkanTestDevice::Create(); CHECK(gpu != nullptr); if(!gpu) return;
  auto d = gpu->device();
  const uint32_t w=192,h=128;
  auto color=Image(d,w,h,nvrhi::Format::RGBA8_UNORM,true);
  auto depth=Image(d,w,h,nvrhi::Format::R32_FLOAT);
  auto map=Image(d,64,64,nvrhi::Format::R32_FLOAT);
  std::vector<float> mapDepth(64*64,1.0f);
  Upload(d,map,mapDepth.data(),64*4);
  auto shadow=SunMap(map);
  std::vector<uint32_t> rgb(w*h,0xB3333333);
  std::vector<float> z(w*h,0.9975f); // 20 units, near=0.05
  for(uint32_t y=0;y<h/3;y++) for(uint32_t x=w/3;x<2*w/3;x++) {
    rgb[y*w+x]=0xB3FFFFFF; z[y*w+x]=1.0f;
  }
  for(uint32_t y=h/2;y<h;y++)for(uint32_t x=w/2-2;x<w/2+2;x++) z[y*w+x]=0.9f; // clear foreground at 0.5 units
  Upload(d,depth,z.data(),w*4);
  LightingConfig config; config.volumetrics=Volumetrics::kHigh;
  SceneLighting lighting(d,config); CHECK(lighting.Initialize());
  SceneLighting::VolumetricInput in; in.color=color;in.depth=depth;in.px=in.py=1;
  in.shadow=&shadow;
  in.fog.color[0]=in.fog.color[1]=in.fog.color[2]=0.3f;
  in.fog.params[0]=0.04f;in.fog.params[2]=1;in.fog.params[3]=0.04f;
  in.fog.normalizing[0]=-20;in.fog.normalizing[1]=-20;in.fog.normalizing[2]=1;
  auto render=[&](float density, bool lit, bool hasMap=true) {
    Upload(d,color,rgb.data(),w*4); in.fog.params[2]=density;
    std::fill(mapDepth.begin(),mapDepth.end(),lit ? 1.0f : 0.0f);
    Upload(d,map,mapDepth.data(),64*4);
    in.shadow=hasMap ? &shadow : nullptr;
    auto before=gpu->ReadBack(color);
    auto cl=d->createCommandList();cl->open(); bool applied=lighting.Volumetric(cl,in);
    cl->close();d->executeCommandList(cl);d->waitForIdle();
    CHECK(applied == (density>0 && hasMap && config.volumetrics_on() && config.volumetric_strength>0));
    auto after=gpu->ReadBack(color);
    float maxGain=0;
    for(size_t i=0;i<after.rgba.size();i+=4) {
      for(int c=0;c<3;c++) {CHECK(after.rgba[i+c]>=before.rgba[i+c]); maxGain=std::max(maxGain,after.rgba[i+c]-before.rgba[i+c]);}
      CHECK_EQ(after.rgba[i+3],before.rgba[i+3]);
      if(z[i/4]>=1.0f || z[i/4]<=0.9f) for(int c=0;c<4;c++) CHECK_EQ(after.rgba[i+c],before.rgba[i+c]);
      for(int c=0;c<3;c++) CHECK(after.rgba[i+c]-before.rgba[i+c]<=0.35f*std::max(before.rgba[i+c],0.03f)+1.0f/255);
    }
    std::printf("shafts density %.2f lit %d map %d: mean gain %.6f, max %.6f\n",density,int(lit),int(hasMap),Mean(after)-Mean(before),maxGain);
    if(density==0 || !lit || !hasMap) CHECK_NEAR(maxGain,0,1e-6);
    return Mean(after)-Mean(before);
  };
  for(auto level : {Volumetrics::kLow,Volumetrics::kHigh}) {
    config.volumetrics=level;lighting.SetConfig(config);
    // Dense fog reaches the receiver bound; keep the thin sample unsaturated.
    float dense=render(1,true),thin=render(0.125f,true);
    CHECK(dense>0.01f);CHECK(dense>thin*2);render(0,true);render(1,false);render(1,true,false);
    // A lit edge texel outside the map cannot illuminate uncovered geometry.
    shadow.light[3]=100.0f;
    CHECK_NEAR(render(1,true),0,1e-6);
    shadow=SunMap(map);
  }
  config.volumetric_strength=0;lighting.SetConfig(config);CHECK_NEAR(render(1,true),0,1e-6);
  config.volumetric_strength=1;config.volumetrics=Volumetrics::kOff;lighting.SetConfig(config);
  CHECK_NEAR(render(1,true),0,1e-6);
  CHECK_EQ(gpu->errors(),0);
}

TEST(Lighting_SunDirectionRejectsDegenerateLight) {
  auto q=SunMap(nullptr);
  float sun[3]={};
  CHECK(SunDirectionFromShadow(q.light,-20,-20,1,1,sun));
  CHECK(sun[2]>0.999f);
  CHECK(!SunDirectionFromShadow(q.light,-20,-20,0,1,sun));
  q.light[12]=0.1f; // perspective light: a single sun direction is not valid
  CHECK(!SunDirectionFromShadow(q.light,-20,-20,1,1,sun));
  q.light[12]=0.0f;
  q.light[10]=-10.0f; // no light-depth gradient: orientation cannot be recovered
  CHECK(!SunDirectionFromShadow(q.light,-20,-20,1,1,sun));
  q={};
  CHECK(!SunDirectionFromShadow(q.light,-20,-20,1,1,sun));
}

TEST(Lighting_ShadowFiltersKeepLitAndBlockedRegionsAndReceiverPlane) {
  auto gpu=VulkanTestDevice::Create();CHECK(gpu!=nullptr);if(!gpu)return;
  auto d=gpu->device();
  constexpr uint32_t w=128,h=64;
  auto map=Image(d,w,h,nvrhi::Format::R32_FLOAT);
  auto target=Image(d,w,h,nvrhi::Format::RGBA8_UNORM,true);
  nvrhi::ShaderDesc sd;sd.shaderType=nvrhi::ShaderType::Vertex;sd.entryName="vs_main";
  auto vs=d->createShader(sd,lighting_filter_vs,sizeof(lighting_filter_vs));
  using I=nvrhi::BindingLayoutItem;using S=nvrhi::BindingSetItem;
  nvrhi::BindingLayoutDesc ld;ld.visibility=nvrhi::ShaderType::All;
  ld.bindings={I::Texture_SRV(0),I::Sampler(0),I::VolatileConstantBuffer(0)};
  auto layout=d->createBindingLayout(ld);
  struct Constants { float shadow[4]={0.6f,3,300,2}; float shape[4]={0.01f,0.35f,2,0}; float surface[4]={0.5f,0,0,0}; } k;
  nvrhi::BufferDesc bd;bd.byteSize=sizeof(k);bd.isConstantBuffer=true;bd.isVolatile=true;bd.maxVersions=16;
  auto cb=d->createBuffer(bd);
  nvrhi::SamplerDesc sam;sam.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
  auto sampler=d->createSampler(sam);
  nvrhi::BindingSetDesc bs;bs.bindings={S::Texture_SRV(0,map),S::Sampler(0,sampler),S::ConstantBuffer(0,cb)};
  auto set=d->createBindingSet(bs,layout);
  nvrhi::FramebufferDesc fd;fd.addColorAttachment(target);auto fb=d->createFramebuffer(fd);
  for(bool pcss : {false,true}) {
    sd.shaderType=nvrhi::ShaderType::Pixel;sd.entryName="ps_main";
    auto ps=pcss ? d->createShader(sd,lighting_filter_pcss,sizeof(lighting_filter_pcss)) :
                   d->createShader(sd,lighting_filter_pcf,sizeof(lighting_filter_pcf));
    nvrhi::GraphicsPipelineDesc pd;pd.VS=vs;pd.PS=ps;pd.bindingLayouts={layout};
    pd.renderState.depthStencilState.depthTestEnable=false;pd.renderState.depthStencilState.depthWriteEnable=false;
    pd.renderState.rasterState.cullMode=nvrhi::RasterCullMode::None;
    auto pipe=d->createGraphicsPipeline(pd,fb->getFramebufferInfo());CHECK(pipe!=nullptr);
    for(bool plane : {false,true}) {
      std::vector<float> depths(w*h,1.0f);
      k.surface[1]=plane ? 0.3f : 0.0f;k.surface[2]=plane ? 0.2f : 0.0f;
      for(uint32_t y=0;y<h;y++)for(uint32_t x=0;x<w;x++)
        depths[y*w+x]=plane ? 0.502f+0.3f*((x+0.5f)/w-0.5f)+0.2f*((y+0.5f)/h-0.5f) : (x<w/2 ? 0.2f : 0.8f);
      Upload(d,map,depths.data(),w*4);
      auto cl=d->createCommandList();cl->open();cl->writeBuffer(cb,&k,sizeof(k));
      nvrhi::GraphicsState gs;gs.pipeline=pipe;gs.framebuffer=fb;gs.bindings={set};
      gs.viewport.addViewport(nvrhi::Viewport(float(w),float(h)));gs.viewport.addScissorRect(nvrhi::Rect(0,w,0,h));
      cl->setGraphicsState(gs);nvrhi::DrawArguments args;args.vertexCount=3;cl->draw(args);cl->close();
      d->executeCommandList(cl);d->waitForIdle();auto pixels=gpu->ReadBack(target);
      for(uint32_t y=8;y<h-8;y++)for(uint32_t x=8;x<w-8;x++) {
        float v=pixels.at(x,y)[0];CHECK(v>=0 && v<=1);
        if(plane || x>w/2+8) CHECK_NEAR(v,1.0f,1.0f/255);
        else if(x<w/2-8) CHECK_NEAR(v,0.0f,1.0f/255);
      }
    }
  }
  CHECK_EQ(gpu->errors(),0);
}
namespace {
// The synthetic water scene (lighting_trace.hlsl): a wall at z=6 above the waterline, empty elsewhere,
// over a water plane at y=-1 traced with the production reflection march.
struct WaterScene {
  VulkanTestDevice* gpu=nullptr;nvrhi::IDevice* d=nullptr;uint32_t w=0,h=0,mipCount=1;
  nvrhi::TextureHandle depth,color,target;nvrhi::BufferHandle cb;nvrhi::BindingSetHandle set;
  nvrhi::FramebufferHandle fb;nvrhi::GraphicsPipelineHandle pipe;nvrhi::ShaderHandle vs,ps;
  nvrhi::BindingLayoutHandle layout;nvrhi::SamplerHandle sam;
  kkshaders::LightingConstants k={};
  // striped: the wall has vertical stripes (8 px) so that a broken reflection shows.
  bool Create(VulkanTestDevice* g,uint32_t width,uint32_t height,bool striped) {
    gpu=g;d=g->device();w=width;h=height;
    for(uint32_t mw=w,mh=h;(mw>1||mh>1)&&mipCount<12;++mipCount){mw=std::max(mw/2,1u);mh=std::max(mh/2,1u);}
    depth=Image(d,w,h,nvrhi::Format::RGBA32_FLOAT,false,mipCount);
    color=Image(d,w,h,nvrhi::Format::RGBA8_UNORM);
    target=Image(d,w,h,nvrhi::Format::RGBA8_UNORM,true);
    std::vector<float> zs(w*h*4,1000);
    std::vector<uint32_t> rgb(w*h,0xFF000000);
    for(uint32_t y=h/8;y<h/2;y++) for(uint32_t x=w/4;x<3*w/4;x++) {
      zs[(y*w+x)*4]=6;rgb[y*w+x]=striped && ((x/8)&1) ? 0xFF102040u : 0xFF4080FFu;
    }
    // Use the same nearest-depth hierarchy as production. A single mip at 4K
    // cannot cross the picture within the production 64-step budget.
    auto upload=d->createCommandList();upload->open();
    uint32_t mw=w,mh=h;
    for(uint32_t mip=0;mip<mipCount;mip++) {
      upload->writeTexture(depth,0,mip,zs.data(),mw*16);
      const uint32_t nw=std::max(mw/2,1u),nh=std::max(mh/2,1u);
      std::vector<float> next(nw*nh*4,1000);
      for(uint32_t y=0;y<nh;y++)for(uint32_t x=0;x<nw;x++)
        for(uint32_t j=0;j<((y==nh-1 && (mh&1)) ? 3u : 2u);j++)
          for(uint32_t i=0;i<((x==nw-1 && (mw&1)) ? 3u : 2u);i++)
          next[(y*nw+x)*4]=std::min(next[(y*nw+x)*4],zs[(std::min(2*y+j,mh-1)*mw+std::min(2*x+i,mw-1))*4]);
      zs=std::move(next);mw=nw;mh=nh;
    }
    upload->close();d->executeCommandList(upload);d->waitForIdle();
    Upload(d,color,rgb.data(),w*4);
    nvrhi::ShaderDesc sd;sd.shaderType=nvrhi::ShaderType::Vertex;sd.entryName="vs_main";
    vs=d->createShader(sd,lighting_trace_vs,sizeof(lighting_trace_vs));
    sd.shaderType=nvrhi::ShaderType::Pixel;sd.entryName="ps_main";
    ps=d->createShader(sd,lighting_trace_ps,sizeof(lighting_trace_ps));
    using I=nvrhi::BindingLayoutItem;using S=nvrhi::BindingSetItem;
    nvrhi::BindingLayoutDesc ld;ld.visibility=nvrhi::ShaderType::All;
    ld.bindings={I::Texture_SRV(0),I::Texture_SRV(1),I::Sampler(0),I::VolatileConstantBuffer(5)};
    layout=d->createBindingLayout(ld);
    nvrhi::BufferDesc bd;bd.byteSize=sizeof(kkshaders::LightingConstants);bd.isConstantBuffer=true;bd.isVolatile=true;bd.maxVersions=16;
    cb=d->createBuffer(bd);
    nvrhi::SamplerDesc sampler;sampler.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    sam=d->createSampler(sampler);
    nvrhi::BindingSetDesc bs;bs.bindings={S::Texture_SRV(0,depth),S::Texture_SRV(1,color),S::Sampler(0,sam),S::ConstantBuffer(5,cb)};
    set=d->createBindingSet(bs,layout);
    nvrhi::FramebufferDesc fd;fd.addColorAttachment(target);fb=d->createFramebuffer(fd);
    nvrhi::GraphicsPipelineDesc pd;pd.VS=vs;pd.PS=ps;pd.bindingLayouts={layout};
    pd.renderState.depthStencilState.depthTestEnable=false;pd.renderState.depthStencilState.depthWriteEnable=false;
    pd.renderState.rasterState.cullMode=nvrhi::RasterCullMode::None;
    pipe=d->createGraphicsPipeline(pd,fb->getFramebufferInfo());
    k.ssrTextures[2]=mipCount;k.ssrTextures[3]=1;
    k.ssrRect[2]=float(w);k.ssrRect[3]=float(h);k.ssrSampler[1]=1;
    k.ssrDepth[0]=0.05f;k.ssrDepth[1]=1;k.ssrDepth[2]=0.05f;k.ssrDepth[3]=1000;
    k.ssrScreen[0]=2;k.ssrScreen[1]=-2;k.ssrScreen[2]=-1;k.ssrScreen[3]=1;
    k.ssrParams[0]=1;k.ssrParams[1]=3;k.ssrParams[2]=0.03f;k.ssrParams[3]=64;
    k.ssrMisc[0]=0.08f;k.ssrMisc[3]=0.02f;
    return pipe!=nullptr;
  }
  // frames > 5: a timed run (milliseconds per frame in *ms, the first five left out).
  Pixels Render(int frames=1,double* ms=nullptr) {
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
    if(ms)*ms=frames>5 ? sum/(frames-5) : 0;
    return gpu->ReadBack(target);
  }
  // The smooth trace's settings as the renderer writes them (game_lighting.cpp); with planes, the
  // water's model planes are the view axes (its up, 0 1 0, is the zx plane's normal).
  void Smooth(bool on,float ripple,bool planes) {
    std::memset(k.ssrAxis,0,sizeof(k.ssrAxis));
    if(planes){k.ssrAxis[0][0]=1;k.ssrAxis[0][3]=1;k.ssrAxis[1][1]=1;k.ssrAxis[1][3]=1;k.ssrAxis[2][2]=1;k.ssrAxis[2][3]=1;}
    k.ssrSmooth[0]=ripple;k.ssrSmooth[1]=on ? 1.0f : 0.0f;k.ssrSmooth[2]=1;k.ssrSmooth[3]=0.75f;
    k.ssrMisc[0]=on ? 0.12f : 0.08f;
  }
};
// Shards: the share of neighbouring water pixels (across and down) whose reflection differs by more
// than about a fifth of the wall's contrast, among the pixels the reflection reaches.
float ShardShare(const Pixels& on,const Pixels& off,uint32_t w,uint32_t h,int* reached=nullptr) {
  int edges=0,count=0;
  auto refl=[&](uint32_t x,uint32_t y,int c){return on.at(x,y)[c]-off.at(x,y)[c];};
  for(uint32_t y=h*3/5;y+1<h;y++)for(uint32_t x=0;x+1<w;x++) {
    if(refl(x,y,0)<=0.02f && refl(x+1,y,0)<=0.02f && refl(x,y+1,0)<=0.02f) continue;
    ++count;
    for(int c=0;c<3;c++)
      if(std::fabs(refl(x,y,c)-refl(x+1,y,c))>0.08f || std::fabs(refl(x,y,c)-refl(x,y+1,c))>0.08f){++edges;break;}
  }
  if(reached)*reached=count;
  return count ? float(edges)/count : 1.0f;
}
}
TEST(Lighting_WaterPlaneReflectsBrightObject) {
  auto gpu=VulkanTestDevice::Create();CHECK(gpu!=nullptr);if(!gpu)return;
  const char* width=std::getenv("NR_WATER_BENCH_WIDTH");
  const uint32_t w=width ? uint32_t(std::atoi(width)) : 192,h=width ? w*9/16 : 128;
  WaterScene s;CHECK(s.Create(gpu.get(),w,h,false));
  // NR_WATER_BENCH_SMOOTH=0: the first (faceted) trace; otherwise the smooth one, as shipped.
  const char* smooth=std::getenv("NR_WATER_BENCH_SMOOTH");
  s.Smooth(!(smooth && smooth[0]=='0'),0.25f,true);
  auto render=[&](bool valid) {
    s.k.ssrTextures[3]=valid;double ms=0;
    auto p=s.Render(width ? 25 : 1,&ms);
    if(width)std::printf("water trace bench %ux%u %s %s: %.3f ms\n",w,h,valid ? "on" : "off",s.k.ssrSmooth[1]>0 ? "smooth" : "faceted",ms);
    return p;
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

// Low-poly waves (flat per triangle) broke the reflection into shards along the triangle edges. The
// smooth trace (the water's flat plane, a share of the tilt, blur) keeps it whole; without a known plane
// it falls back to the triangles' own normals; on flat water it adds no edges.
TEST(Lighting_FacetedWaterReflectionStaysWhole) {
  auto gpu=VulkanTestDevice::Create();CHECK(gpu!=nullptr);if(!gpu)return;
  const uint32_t w=384,h=256;
  WaterScene s;CHECK(s.Create(gpu.get(),w,h,true));
  auto shards=[&](float waves,bool smooth,float ripple,bool planes,int* reached) {
    s.k.shadow[0]=waves;s.Smooth(smooth,ripple,planes);
    s.k.ssrTextures[3]=0;auto off=s.Render();
    s.k.ssrTextures[3]=1;auto on=s.Render();
    return ShardShare(on,off,w,h,reached);
  };
  int nFlat=0,nFlatSmooth=0,nFaceted=0,nSmooth=0,nCalm=0,nNoPlane=0;
  const float flat=shards(0,false,0.25f,true,&nFlat);
  const float flatSmooth=shards(0,true,0.25f,true,&nFlatSmooth);
  const float faceted=shards(0.04f,false,0.25f,true,&nFaceted);
  const float smooth=shards(0.04f,true,0.25f,true,&nSmooth);
  const float calm=shards(0.04f,true,0.0f,true,&nCalm);
  const float noPlane=shards(0.04f,true,0.25f,false,&nNoPlane);
  std::printf("faceted water shards: flat %.4f (%d px) / smooth %.4f (%d); waves: faceted trace %.4f (%d), "
              "smooth ripple 0.25 %.4f (%d), ripple 0 %.4f (%d), no plane %.4f (%d)\n",
              flat,nFlat,flatSmooth,nFlatSmooth,faceted,nFaceted,smooth,nSmooth,calm,nCalm,noPlane,nNoPlane);
  CHECK(nFlat>500);CHECK(nFlatSmooth>500);CHECK(nSmooth>500);CHECK(nFaceted>200);
  // The stripes' own edges count in every case: compare what the waves add.
  CHECK(smooth-flatSmooth<(faceted-flat)*0.5f);  // under half the shard edges the waves added
  CHECK(calm<=smooth+0.01f);      // ripple 0 is the calmest
  CHECK(noPlane>smooth);          // without the plane the triangles' normals stay
  CHECK(flatSmooth<=flat+0.02f);  // flat water: no new edges
  CHECK_EQ(gpu->errors(),0);
}

// The water draw's planes from its world-view-projection: a camera looking down 30 degrees, a water
// model scaled evenly or unevenly (as a stretched water plane would be) gives view-space normals of its
// planes; a projective texture matrix (bias moves the off-centre terms by about a half) before an evenly
// scaled model's matrix is passed over.
TEST(Lighting_WaterPlanesFromTheDrawsMatrix) {
  AoProjection cam;cam.px=1.2f;cam.py=1.8f;cam.cx=0;cam.cy=0;cam.a=1.0001f;cam.b=-0.10001f;
  CHECK(cam.valid());
  const float c=std::cos(0.5236f),sn=std::sin(0.5236f);
  float rows[16]={};
  // Model to view = R (rows x 1 0 0, y 0 sn c, z 0 c -sn) times the scale, then an offset.
  const float R[3][3]={{1,0,0},{0,sn,c},{0,c,-sn}};
  auto planes=[&](const float S[3],bool texFirst) {
    const float T[3]={1,-3,10};
    float M[3][4];
    for(int i=0;i<3;i++){for(int j=0;j<3;j++)M[i][j]=R[i][j]*S[j];M[i][3]=T[i];}
    for(int j=0;j<4;j++){
      rows[0+j]=cam.px*M[0][j]+cam.cx*M[2][j];
      rows[4+j]=cam.py*M[1][j]+cam.cy*M[2][j];
      rows[8+j]=cam.a*M[2][j]+(j==3 ? cam.b : 0.0f);
      rows[12+j]=M[2][j];
    }
    std::vector<uint8_t> be(256*16,0);
    auto store=[&](uint32_t reg,const float* m){
      for(int i=0;i<16;i++){uint32_t u;std::memcpy(&u,&m[i],4);
        for(int b=0;b<4;b++)be[(reg*4+i)*4+b]=uint8_t(u>>(24-8*b));}
    };
    // A projective texture matrix (0.5 x + 0.5 w, -0.5 y + 0.5 w) before or after the camera's.
    float tex[16];
    for(int j=0;j<4;j++){tex[j]=0.5f*rows[j]+0.5f*rows[12+j];tex[4+j]=-0.5f*rows[4+j]+0.5f*rows[12+j];tex[8+j]=rows[8+j];tex[12+j]=rows[12+j];}
    if(texFirst){store(8,tex);store(20,rows);}else{store(0,rows);store(8,tex);}
    float out[3][4];
    CHECK(WaterPlanesFromConstants(cam,be.data(),out));
    // The model's xy plane (world z up) in view space is R's third column: (0, c, -sn), up to sign.
    const float up[3]={R[0][2],R[1][2],R[2][2]};
    const float d=out[2][0]*up[0]+out[2][1]*up[1]+out[2][2]*up[2];
    std::printf("water planes: xy plane normal (%.4f %.4f %.4f), dot with up %.6f\n",out[2][0],out[2][1],out[2][2],d);
    CHECK(std::fabs(std::fabs(d)-1.0f)<1e-4f);CHECK_EQ(out[2][3],1.0f);
    CHECK(std::fabs(out[0][0]*R[0][0]+out[0][1]*R[1][0]+out[0][2]*R[2][0])>0.9999f);  // yz plane: model x
  };
  const float even[3]={3,3,3},uneven[3]={4,2,1};
  planes(even,true);
  planes(uneven,false);
  std::vector<uint8_t> none(256*16,0);float out[3][4];
  CHECK(!WaterPlanesFromConstants(cam,none.data(),out));CHECK_EQ(out[1][3],0.0f);
}

TEST(Lighting_ShaftCost) {
  if (!std::getenv("NR_LIGHTING_BENCH")) return;
  auto gpu=VulkanTestDevice::Create(false);CHECK(gpu!=nullptr);if(!gpu)return;
  auto d=gpu->device();
  for(uint32_t w : {1920u,2560u}) {
    uint32_t h=w*9/16;
    auto color=Image(d,w,h,nvrhi::Format::RGBA8_UNORM,true);
    auto depth=Image(d,w,h,nvrhi::Format::R32_FLOAT);
    auto map=Image(d,832,832,nvrhi::Format::R32_FLOAT);
    std::vector<float> mapDepth(832*832,1.0f);
    Upload(d,map,mapDepth.data(),832*4);
    auto shadow=SunMap(map);
    std::vector<uint32_t> rgb(w*h,0xFF333333);std::vector<float> z(w*h,0.9975f);
    for(uint32_t y=0;y<h/3;y++)for(uint32_t x=w/3;x<2*w/3;x++){rgb[y*w+x]=0xFFFFFFFF;z[y*w+x]=1;}
    Upload(d,depth,z.data(),w*4);
    for(auto level : {Volumetrics::kLow,Volumetrics::kHigh}) {
      LightingConfig config;config.volumetrics=level;SceneLighting lighting(d,config);CHECK(lighting.Initialize());
      SceneLighting::VolumetricInput in;in.color=color;in.depth=depth;in.px=in.py=1;
      in.shadow=&shadow;
      in.fog.color[0]=in.fog.color[1]=in.fog.color[2]=0.3f;
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
TEST(Lighting_SsrPreparationCost) {
  if (!std::getenv("NR_LIGHTING_BENCH")) return;
  auto gpu=VulkanTestDevice::Create(false);CHECK(gpu!=nullptr);if(!gpu)return;
  auto d=gpu->device();
  for(uint32_t w : {1920u,2560u}) {
    const uint32_t h=w*9/16;
    auto color=Image(d,w,h,nvrhi::Format::RGBA8_UNORM,true);
    auto depth=Image(d,w,h,nvrhi::Format::R32_FLOAT);
    std::vector<uint32_t> rgb(w*h,0xFF333333);std::vector<float> z(w*h,0.9975f);
    Upload(d,color,rgb.data(),w*4);Upload(d,depth,z.data(),w*4);
    // An ordinary perspective matrix, in the game's big-endian constants.
    float matrix[16]={1,0,0,0, 0,1,0,0, 0,0,1.00005f,-0.0500025f, 0,0,1,0};
    uint8_t constants[4096]={};
    for(int i=0;i<16;i++) {
      uint32_t bits;std::memcpy(&bits,matrix+i,4);
      for(int j=0;j<4;j++) constants[i*4+j]=uint8_t(bits>>(24-j*8));
    }
    LightingConfig config;config.water=WaterReflections::kSsr;
    SceneLighting lighting(d,config);CHECK(lighting.Initialize());
    SceneLighting::Draw draw;draw.color={1,color};draw.depth={2,depth};draw.frame_size=true;
    draw.vs_constants=constants;
    draw.screen.x_scale=draw.screen.x_offset=w/2.0f;
    draw.screen.y_scale=-float(h)/2;draw.screen.y_offset=h/2.0f;
    draw.screen.z_scale=1;draw.screen.half_pixel=0.5f;
    draw.state.color=draw.state.depth=draw.state.depth_test=true;
    draw.state.blend_control=0x00010001;draw.state.primitive=4;
    auto tq=d->createTimerQuery();double sum=0;
    for(int frame=0;frame<25;frame++) {
      auto cl=d->createCommandList();cl->open();draw.frame=frame;
      draw.family=LightFamily::kNone;draw.state.ps_hash=0;draw.state.depth_write=true;
      lighting.OnDraw(cl,draw);
      draw.family=LightFamily::kWater;draw.state.ps_hash=0x805E38F150347CFAull;draw.state.depth_write=false;
      cl->beginTimerQuery(tq);lighting.OnDraw(cl,draw);cl->endTimerQuery(tq);lighting.OnPresent(cl);
      cl->close();d->executeCommandList(cl);d->waitForIdle();
      CHECK(lighting.ssr().valid);CHECK_EQ(lighting.ssr().width,w);CHECK_EQ(lighting.ssr().height,h);
      if(frame>=5)sum+=d->getTimerQueryTime(tq)*1000;
      d->resetTimerQuery(tq);d->runGarbageCollection();
    }
    std::printf("SSR preparation bench %ux%u: %.3f ms (depth + colour + all mips)\n",w,h,sum/20);
  }
}
NR_TEST_MAIN()
