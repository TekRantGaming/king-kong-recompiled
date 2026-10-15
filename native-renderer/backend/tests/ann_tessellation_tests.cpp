// Task 072: actual packed Ann stages on synthetic vertices, never a game
// process. Arguments: a pack produced by `kkshaders ann-tess`, DXC folder.
// NR_BENCH=1 times 21 overlapping synthetic triangles, not a game FPS
// prediction or an Ann mesh.
#include "backend/pipeline_cache.h"
#include "backend/pipeline_prewarm.h"
#include "backend/tests/check.h"
#include "backend/tests/vk_test_device.h"
#include "kkshaders/abi.h"
#include "kkshaders/tessellation.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#if NR_HAS_D3D12
#include <d3d12sdklayers.h>
#include <nvrhi/d3d12.h>
#include <nvrhi/validation.h>
#include <wrl/client.h>
#endif

using namespace nr::test;
namespace {
constexpr uint64_t hashes[]={0x5E779DBA373D121Cull,0x3390D930CD168AC0ull,
0xD710A475A7840F37ull,0xC66476D2889954ECull,0x7149CE3656F92235ull,
0xA6E7AF900E6F2741ull,0x91632BD5704802BDull,0x448D8A65A65954D8ull};
constexpr int projects[]={52,36,144,36,160,144,52,36};
struct Harness {
  std::unique_ptr<VulkanTestDevice> host;
#if NR_HAS_D3D12
  struct Messages : nvrhi::IMessageCallback {
    int errors = 0;
    void message(nvrhi::MessageSeverity s, const char *t) override {
      if (s == nvrhi::MessageSeverity::Error ||
          s == nvrhi::MessageSeverity::Fatal) {
        ++errors;
        std::fprintf(stderr, "%s\n", t);
      }
    }
  } messages;
  Microsoft::WRL::ComPtr<ID3D12Device> native;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
  nvrhi::DeviceHandle dxDevice;
#endif
  bool directx = false;
  nvrhi::IDevice *device = nullptr;
  kkshaders::ShaderPack pack;
  nvrhi::TextureHandle color, depth;
  nvrhi::FramebufferHandle framebuffer;
  nvrhi::BufferHandle vc, pc, dc, sc, previous, vertices;
  std::array<nvrhi::BindingLayoutHandle, 7> layouts;
  nvrhi::BindingSetHandle constants;
  std::array<nvrhi::DescriptorTableHandle, 6> tables;
  nvrhi::ShaderHandle pixel, motionPixel;
  nvrhi::ShaderHandle materialPixel[8];
  float floats[256][4]{};
  kkshaders::DrawConstants draw{};
  kkshaders::MotionConstants history{};
  struct Stages {
    nvrhi::ShaderHandle base, vs, hs, ds, mvs, mhs, mds;
  } stages[8];

  nvrhi::ShaderHandle Load(uint64_t hash, uint64_t input,
                           nvrhi::ShaderType type, const char *entry) {
    kkshaders::CompiledShader shader;
    if (!pack.find(hash, input, shader))
      return nullptr;
    const auto &blob = directx ? shader.dxil : shader.spirv;
    if (blob.empty())
      return nullptr;
    nvrhi::ShaderDesc desc;
    desc.shaderType = type;
    desc.entryName = entry;
    return device->createShader(desc, blob.data(), blob.size());
  }
  void Execute(nvrhi::ICommandList *cl) {
    if (host)
      host->Execute(cl);
    else
      device->executeCommandList(cl);
  }
  void WaitIdle() {
    if (host)
      host->WaitIdle();
    else
      device->waitForIdle();
  }
  Pixels ReadBack() { return nr::test::ReadBackTexture(device, color); }
  int Errors() const {
#if NR_HAS_D3D12
    if (directx) {
      int count = messages.errors;
      Microsoft::WRL::ComPtr<ID3D12InfoQueue> info;
      if (SUCCEEDED(native.As(&info)))
        for (UINT64 i = 0;
             i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); i++) {
          SIZE_T length = 0;
          info->GetMessage(i, nullptr, &length);
          std::vector<uint8_t> bytes(length);
          auto *message = reinterpret_cast<D3D12_MESSAGE *>(bytes.data());
          if (SUCCEEDED(info->GetMessage(i, message, &length)) &&
              (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
               message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)) {
            ++count;
            std::fprintf(stderr, "D3D12: %s\n", message->pDescription);
          }
        }
      return count;
    }
#endif
    return host->errors();
  }
  bool Init(const char *path, const char *dxc, bool dx) {
    std::string error;
    if (!kkshaders::loadDxc(dxc, &error) || !pack.open(path, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return false;
    }
    directx = dx;
#if NR_HAS_D3D12
    if (dx) {
      Microsoft::WRL::ComPtr<ID3D12Debug> debug;
      if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();
      if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
                                   IID_PPV_ARGS(&native))))
        return false;
      D3D12_COMMAND_QUEUE_DESC q{};
      if (FAILED(native->CreateCommandQueue(&q, IID_PPV_ARGS(&queue))))
        return false;
      nvrhi::d3d12::DeviceDesc d;
      d.pDevice = native.Get();
      d.pGraphicsCommandQueue = queue.Get();
      d.errorCB = &messages;
      dxDevice = nvrhi::validation::createValidationLayer(
          nvrhi::d3d12::createDevice(d));
      device = dxDevice;
    } else
#endif
    {
      host = VulkanTestDevice::Create(true);
      if (!host)
        return false;
      device = host->device();
    }
    kkshaders::Compiler compiler;
    const auto target =
        dx ? kkshaders::CompileTarget::Dxil : kkshaders::CompileTarget::Spirv;
    auto ps = compiler.compileEntry(
        "float4 main():SV_Target {return float4(1,1,1,1);}", L"ps_6_0", L"main",
        target);
    auto mps = compiler.compileEntry(
        "struct In {float4 pos:SV_Position; float4 c:KKMOTION0; float4 "
        "p:KKMOTION1;}; float4 main(In i):SV_Target {return "
        "float4(abs(i.c.xyz/i.c.w-i.p.xyz/i.p.w),1);}",
        L"ps_6_0", L"main", target);
    nvrhi::ShaderDesc sd;
    sd.shaderType = nvrhi::ShaderType::Pixel;
    sd.entryName = "main";
    pixel = device->createShader(sd, ps.blob.data(), ps.blob.size());
    motionPixel = device->createShader(sd, mps.blob.data(), mps.blob.size());
    for (int v = 0; v < 8; v++) {
      const uint64_t hash = hashes[v];
      kkshaders::CompiledShader base;
      if (!pack.find(hash, 0, base))
        return false;
      auto &s = stages[v];
      s.base = Load(hash, base.inputHash, nvrhi::ShaderType::Vertex, "main");
      s.vs = Load(hash ^ kkshaders::tessPackTag(0, false), base.inputHash,
                  nvrhi::ShaderType::Vertex, "ktVS");
      s.hs = Load(hash ^ kkshaders::tessPackTag(1, false), base.inputHash,
                  nvrhi::ShaderType::Hull, "ktHS");
      s.ds = Load(hash ^ kkshaders::tessPackTag(2, false), base.inputHash,
                  nvrhi::ShaderType::Domain, "ktDS");
      s.mvs = Load(hash ^ kkshaders::tessPackTag(0, true), base.inputHash,
                   nvrhi::ShaderType::Vertex, "ktVS");
      s.mhs = Load(hash ^ kkshaders::tessPackTag(1, true), base.inputHash,
                   nvrhi::ShaderType::Hull, "ktHS");
      s.mds = Load(hash ^ kkshaders::tessPackTag(2, true), base.inputHash,
                   nvrhi::ShaderType::Domain, "ktDS");
      if (!s.base || !s.vs || !s.hs || !s.ds || !s.mvs || !s.mhs || !s.mds)
        return false;
      if(v==2 || v==3) continue;
      const uint64_t pixels[]={0x7DC46286A4EA2E2Eull,0xF5BCD47A4145F958ull,0,0,
        0x01D62D86BB133315ull,0xBBD239D3BEB254D9ull,0x3A076C4DF061DFA7ull,0x07BA5284CF0C43AEull};
      materialPixel[v] = Load(pixels[v],
                              0, nvrhi::ShaderType::Pixel, "main");
      if (!materialPixel[v])
        return false;
    }
    auto cb = [&](uint64_t bytes) {
      nvrhi::BufferDesc d;
      d.byteSize = bytes;
      d.isConstantBuffer = true;
      d.initialState = nvrhi::ResourceStates::ConstantBuffer;
      d.keepInitialState = true;
      return device->createBuffer(d);
    };
    vc = cb(sizeof(floats));
    pc = cb(sizeof(floats));
    dc = cb(sizeof(draw));
    previous = cb(sizeof(history));
    sc = cb(sizeof(kkshaders::ScaleConstants));
    nvrhi::BindingLayoutDesc layout;
    layout.visibility = nvrhi::ShaderType::All;
    layout.bindingOffsets = nvrhi::VulkanBindingOffsets()
                                .setShaderResourceOffset(0)
                                .setSamplerOffset(0)
                                .setConstantBufferOffset(0)
                                .setUnorderedAccessViewOffset(0);
    layout.bindings = {nvrhi::BindingLayoutItem::ConstantBuffer(0),
                       nvrhi::BindingLayoutItem::ConstantBuffer(1),
                       nvrhi::BindingLayoutItem::ConstantBuffer(2),
                       nvrhi::BindingLayoutItem::ConstantBuffer(3),
                       nvrhi::BindingLayoutItem::ConstantBuffer(5)};
    layouts[0] = device->createBindingLayout(layout);
    nvrhi::BindingSetDesc set;
    set.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, vc),
                    nvrhi::BindingSetItem::ConstantBuffer(1, pc),
                    nvrhi::BindingSetItem::ConstantBuffer(2, dc),
                    nvrhi::BindingSetItem::ConstantBuffer(3, sc),
                    nvrhi::BindingSetItem::ConstantBuffer(5, previous)};
    constants = device->createBindingSet(set, layouts[0]);
    for (unsigned i = 1; i <= 6; i++) {
      nvrhi::BindlessLayoutDesc b;
      b.visibility = nvrhi::ShaderType::All;
      b.maxCapacity = 1;
      b.registerSpaces.push_back(
          i == 5   ? nvrhi::BindingLayoutItem::Sampler(i)
          : i == 6 ? nvrhi::BindingLayoutItem::RawBuffer_SRV(i)
                   : nvrhi::BindingLayoutItem::Texture_SRV(i));
      layouts[i] = device->createBindlessLayout(b);
      tables[i - 1] = device->createDescriptorTable(layouts[i]);
      device->resizeDescriptorTable(tables[i - 1], 1, false);
    }
    float data[63][7][4]{};
    const float xy[3][2] = {{-0.8f, -0.8f}, {0.8f, -0.8f}, {0.0f, 0.8f}};
    for (int i = 0; i < 63; i++) {
      auto &v = data[i];
      v[0][0] = xy[i % 3][0];
      v[0][1] = xy[i % 3][1];
      v[0][2] = 0.5f;
      v[0][3] = 1;
      v[1][0] = xy[i % 3][0] * 0.6f;
      v[1][1] = xy[i % 3][1] * 0.6f;
      v[1][2] = -1;
      v[2][0] = v[2][1] = v[2][2] = v[2][3] = 1;
      v[3][0] = 0.65f; v[3][1] = 0.35f; v[4][1] = 1.0f;
      v[6][0] = 1;
      v[6][3] = 1;
    }
    nvrhi::BufferDesc vb;
    vb.byteSize = sizeof(data);
    vb.canHaveRawViews = true;
    vb.initialState = nvrhi::ResourceStates::ShaderResource;
    vb.keepInitialState = true;
    vertices = device->createBuffer(vb);
    device->writeDescriptorTable(
        tables[5], nvrhi::BindingSetItem::RawBuffer_SRV(0, vertices));
    auto upload = device->createCommandList();
    upload->open();
    upload->writeBuffer(vertices, data, sizeof(data));
    upload->close();
    Execute(upload);
    WaitIdle();
    for (int i = 0; i < 7; i++) {
      draw.vertexFetch[i][1] = i * 16;
      draw.vertexFetch[i][2] = 7 * 16;
      draw.vertexFetch[i][3] = 38u | (0x688u << 17);
    }
    draw.loopConstants[0] = 2 | (1u<<16);
    draw.ndcScale[0] = 1;
    draw.ndcScale[1] = 1;
    draw.ndcScale[2] = 1;
    nvrhi::TextureDesc td;
    td.width = td.height = 256;
    td.format = nvrhi::Format::RGBA8_UNORM;
    td.isRenderTarget = true;
    td.initialState = nvrhi::ResourceStates::RenderTarget;
    td.keepInitialState = true;
    color = device->createTexture(td);
    td.format = nvrhi::Format::D32;
    td.initialState = nvrhi::ResourceStates::DepthWrite;
    depth = device->createTexture(td);
    nvrhi::FramebufferDesc fb;
    fb.addColorAttachment(color).setDepthAttachment(depth);
    framebuffer = device->createFramebuffer(fb);
    return pixel && motionPixel && constants && framebuffer;
  }

  nvrhi::GraphicsPipelineHandle Pipeline(int variant, int detail,
                                         bool motion = false,
                                         bool depthOnly = false,
                                         bool material = false) {
    auto &s = stages[variant];
    nvrhi::GraphicsPipelineDesc d;
    d.primType = detail ? nvrhi::PrimitiveType::PatchList
                        : nvrhi::PrimitiveType::TriangleList;
    d.patchControlPoints = detail ? 3 : 0;
    d.VS = detail ? (motion ? s.mvs : s.vs) : s.base;
    if (detail) {
      d.HS = motion ? s.mhs : s.hs;
      d.DS = motion ? s.mds : s.ds;
    }
    if (!depthOnly)
      d.PS = motion ? motionPixel : material ? materialPixel[variant] : pixel;
    for (auto &l : layouts)
      d.bindingLayouts.push_back(l);
    d.renderState.rasterState.cullMode = nvrhi::RasterCullMode::None;
    d.renderState.depthStencilState.depthTestEnable = motion || depthOnly;
    d.renderState.depthStencilState.depthWriteEnable = depthOnly;
    d.renderState.depthStencilState.depthFunc =
        motion ? nvrhi::ComparisonFunc::Equal : nvrhi::ComparisonFunc::Always;
    d.renderState.depthStencilState.stencilEnable = false;
    d.renderState.blendState.targets[0].colorWriteMask =
        depthOnly ? nvrhi::ColorMask(0) : nvrhi::ColorMask::All;
    auto pipeline =
        device->createGraphicsPipeline(d, framebuffer->getFramebufferInfo());
#if NR_HAS_D3D12
    if (directx && !pipeline) {
      Microsoft::WRL::ComPtr<ID3D12InfoQueue> info;
      if (SUCCEEDED(native.As(&info)))
        for (UINT64 i = 0;
             i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); i++) {
          SIZE_T length = 0;
          info->GetMessage(i, nullptr, &length);
          std::vector<uint8_t> bytes(length);
          auto *message = reinterpret_cast<D3D12_MESSAGE *>(bytes.data());
          if (SUCCEEDED(info->GetMessage(i, message, &length)))
            std::fprintf(stderr, "D3D12: %s\n", message->pDescription);
        }
    }
#endif
    return pipeline;
  }
  void Constants(int variant, int detail, float alpha) {
    std::memset(floats, 0, sizeof(floats));
    floats[0][0] = floats[1][1] = floats[2][2] = 1;
    floats[3][0]=0.95f; floats[3][1]=-0.1f; floats[4][0]=0.1f; floats[4][1]=1.05f; floats[5][2]=1;
    int project = projects[variant], view = variant ? 44 : 56;
    // Depth has POSITION/WEIGHTS/INDICES at 0/1/2 and the paired normal at 15.
    // Lit has the usual 0..6 declaration.
    for(int i=0;i<7;++i) { draw.vertexFetch[i][1]=i*16; draw.vertexFetch[i][2]=112; draw.vertexFetch[i][3]=38u|(0x688u<<17); }
    std::memcpy(draw.vertexFetch[15],draw.vertexFetch[1],16);
    if(variant==2 || variant==3) {
      std::memcpy(draw.vertexFetch[1],draw.vertexFetch[3],16);
      std::memcpy(draw.vertexFetch[2],draw.vertexFetch[4],16);
    }
    for (int i = 0; i < 4; i++)
      floats[project + i][i] = 1;
    if(variant<2) for (int i = 0; i < 3; i++)
      floats[view + i][i] = 1;
    draw.reserved = uint32_t(std::max(1, detail)) |
                    (uint32_t(std::lround(alpha * 65535)) << 8);
    std::memcpy(history.previous, floats, sizeof(floats));
    history.previous[0][3] = history.previous[3][3] = 0.1f;
    std::memcpy(&history.params[3], &draw.reserved, 4);
  }
  void State(nvrhi::ICommandList *cl, nvrhi::IGraphicsPipeline *pipeline) {
    nvrhi::GraphicsState s;
    s.pipeline = pipeline;
    s.framebuffer = framebuffer;
    s.viewport.addViewport(nvrhi::Viewport(256.f, 256.f));
    s.viewport.addScissorRect(nvrhi::Rect(0, 256, 0, 256));
    s.bindings.push_back(constants);
    for (auto &t : tables)
      s.bindings.push_back(t);
    cl->setGraphicsState(s);
    nvrhi::DrawArguments a;
    a.vertexCount = 63;
    cl->draw(a);
  }
  Pixels Render(int variant, int detail, float alpha, bool motion = false) {
    Constants(variant, detail, alpha);
    auto p = Pipeline(variant, detail, motion);
    CHECK(p);
    auto dp = motion ? Pipeline(variant, detail, false, true)
                     : nvrhi::GraphicsPipelineHandle{};
    auto cl = device->createCommandList();
    cl->open();
    cl->writeBuffer(vc, floats, sizeof(floats));
    cl->writeBuffer(pc, floats, sizeof(floats));
    cl->writeBuffer(dc, &draw, sizeof(draw));
    cl->writeBuffer(previous, &history, sizeof(history));
    cl->clearTextureFloat(color, nvrhi::AllSubresources, nvrhi::Color(0));
    cl->clearDepthStencilTexture(depth, nvrhi::AllSubresources, true, 1, false,
                                 0);
    if (dp)
      State(cl, dp);
    State(cl, p);
    cl->close();
    Execute(cl);
    WaitIdle();
    return ReadBack();
  }
  Pixels Paired(int depthVariant, int litVariant, int detail, bool motion) {
    auto dp=Pipeline(depthVariant,detail,false,true);
    auto lp=Pipeline(litVariant,detail,motion);
    // Clone a depth-equal pipeline using lit stages, including ordinary colour replay.
    auto desc=lp->getDesc();
    desc.renderState.depthStencilState.depthTestEnable=true;
    desc.renderState.depthStencilState.depthFunc=nvrhi::ComparisonFunc::Equal;
    lp=device->createGraphicsPipeline(desc,framebuffer->getFramebufferInfo());
    CHECK(dp && lp);
    auto cl=device->createCommandList(); cl->open();
    cl->clearTextureFloat(color,nvrhi::AllSubresources,nvrhi::Color(0));
    cl->clearDepthStencilTexture(depth,nvrhi::AllSubresources,true,1,false,0);
    Constants(depthVariant,detail,0.75f);
    cl->writeBuffer(vc,floats,sizeof(floats)); cl->writeBuffer(dc,&draw,sizeof(draw));
    State(cl,dp);
    Constants(litVariant,detail,0.75f);
    cl->writeBuffer(vc,floats,sizeof(floats)); cl->writeBuffer(dc,&draw,sizeof(draw));
    cl->writeBuffer(previous,&history,sizeof(history));
    State(cl,lp); cl->close(); Execute(cl); WaitIdle(); return ReadBack();
  }
  double Bench(int detail) {
    Constants(0, detail, 0.75f);
    auto p = Pipeline(0, detail);
    auto q = device->createTimerQuery();
    std::vector<double> samples;
    for (int i = 0; i < 20; i++) {
      auto cl = device->createCommandList();
      cl->open();
      cl->writeBuffer(vc, floats, sizeof(floats));
      cl->writeBuffer(pc, floats, sizeof(floats));
      cl->writeBuffer(dc, &draw, sizeof(draw));
      cl->beginTimerQuery(q);
      for (int drawIndex = 0; drawIndex < 64; drawIndex++)
        State(cl, p);
      cl->endTimerQuery(q);
      cl->close();
      Execute(cl);
      WaitIdle();
      if (i >= 5)
        samples.push_back(1000.0 * device->getTimerQueryTime(q) / 64.0);
      device->resetTimerQuery(q);
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
  }
};
} // namespace

int main(int argc, char **argv) {
  if (argc < 3 || argc > 4) {
    std::fprintf(stderr, "usage: nr_ann_tessellation_tests <prepared-pack> "
                         "<dxc-folder> [d3d12]\n");
    return 2;
  }
  const bool dx = argc == 4 && std::strcmp(argv[3], "d3d12") == 0;
  Harness h;
  if (!h.Init(argv[1], argv[2], dx))
    return 1;
  std::printf("API: %s\n", dx ? "D3D12" : "Vulkan");
  for (int variant = 0; variant < 8; variant++) {
    if(variant!=2 && variant!=3) CHECK(h.Pipeline(variant, 4, false, false, true));
    auto off = h.Render(variant, 0, 0);
    for (int detail : {4, 16}) {
      auto zero = h.Render(variant, detail, 0);
      auto smooth = h.Render(variant, detail, 0.75f);
      auto motion = h.Render(variant, detail, 0.75f, true);
      unsigned different = 0, curved = 0, moving = 0;
      for (size_t i = 0; i < off.rgba.size(); i += 4) {
        if (std::abs(off.rgba[i] - zero.rgba[i]) > 0.1f)
          ++different;
        if (std::abs(zero.rgba[i] - smooth.rgba[i]) > 0.1f)
          ++curved;
        if (motion.rgba[i] > 0.08f && motion.rgba[i] < 0.12f)
          ++moving;
      }
      std::printf("variant %d detail %d: alpha-zero coverage changes %u, "
                  "curved pixels %u, depth-equal motion pixels %u\n",
                  variant, detail, different, curved, moving);
      CHECK(different < 32);
      CHECK(curved > 100);
      CHECK(moving > 1000);
    }
  }
  for(int lit=4;lit<8;++lit) for(int detail:{4,16}) {
    auto reference=h.Render(lit,detail,0.75f);
    auto equal=h.Paired(lit<6?2:3,lit,detail,false);
    auto motion=h.Paired(lit<6?2:3,lit,detail,true);
    unsigned covered=0, missed=0, moving=0;
    for(size_t i=0;i<reference.rgba.size();i+=4) {
      if(reference.rgba[i]>0.5f) {++covered; if(equal.rgba[i]<0.5f) ++missed;}
      if(motion.rgba[i]>0.08f && motion.rgba[i]<0.12f) ++moving;
    }
    std::printf("paired depth/lit %d detail %d covered %u missing %u moving %u\n",lit,detail,covered,missed,moving);
    CHECK(covered>1000); CHECK_EQ(missed,0u); CHECK_EQ(moving,covered);
  }
  if (const char *list = std::getenv("NR_TESS_PREWARM_LIST")) {
    const char *filtered = std::getenv("NR_TESS_PREWARM_FILTERED");
    if (!filtered)
      return 2;
    std::vector<nr::PipelineRecord> records;
    CHECK(nr::ReadPipelineList(list, records));
    records.erase(std::remove_if(records.begin(), records.end(),
                                 [](const auto &r) {
                                   if(!kkshaders::annTessellationShader(r.vs_hash)) return true;
                                   if(!r.ps_hash) return false;
                                   const uint64_t pixels[]={0x7DC46286A4EA2E2Eull,0xF5BCD47A4145F958ull,
                                     0x01D62D86BB133315ull,0xBBD239D3BEB254D9ull,0x3A076C4DF061DFA7ull,0x07BA5284CF0C43AEull};
                                   return std::find(std::begin(pixels),std::end(pixels),r.ps_hash)==std::end(pixels);
                                 }),
                  records.end());
    CHECK(nr::WritePipelineList(filtered, records));
    nr::PipelinePrewarm prewarm(h.device);
    nr::PipelinePrewarm::Settings settings;
    settings.shader_pack = argv[1];
    settings.dxc = argv[2];
    settings.shaders_aware = true;
    settings.render_scale_x = settings.render_scale_y = 1.1f;
    settings.tessellation_supported = true;
    settings.upscale.kind = nr::UpscalerKind::kBilinear;
    settings.lists = {filtered};
    CHECK(prewarm.Start(settings));
    CHECK(prewarm.Wait(30));
    auto progress = prewarm.progress();
    std::printf("Production prewarm: %llu queued, %llu made, %llu waiting, %zu "
                "source records\n",
                (unsigned long long)progress.queued,
                (unsigned long long)progress.made,
                (unsigned long long)progress.waiting, records.size());
    CHECK(progress.queued >= 6);
    CHECK(progress.queued == progress.made);
    CHECK(progress.waiting == 0);
    prewarm.Stop();
  }
  if (std::getenv("NR_BENCH"))
    for (int detail : {0, 4, 16})
      std::printf("NR_BENCH Ann stages, synthetic 21 triangles, detail %d: "
                  "%.6f GPU ms/draw\n",
                  detail, h.Bench(detail));
  CHECK_EQ(h.Errors(), 0);
  return Failures() ? 1 : 0;
}
