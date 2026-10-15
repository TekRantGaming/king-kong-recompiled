// Actual renderer upload, descriptor switch, strip copies and budget fallback
// on both native APIs. All pixels and identities are synthetic.
#include "backend/game_renderer.h"
#include "backend/shader_library.h"
#include "backend/tests/fake_guest.h"
#include "backend/tests/vk_test_device.h"
#include "backend/tests/memory_d3d12_test_device.h"
#include "kknr/texture_pack.h"
#include "../../resources/tools/image_io.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
namespace nr {
static void Check(bool ok,const char* message) { if(!ok) { std::fprintf(stderr,"FAIL %s\n",message);std::exit(1); } }
struct TexturePackTestAccess {
  template<class Host> static void Run(Host& host) {
    auto* device=host->device();test::FakeGuestMemory guest;
    kknr::GuestMemory physical{guest.PhysicalBase(),guest.PhysicalSize()};
    ShaderLibrary shaders(device,guest);GameRenderer g(device,guest,physical,&shaders);
    Check(g.Initialize(),"initialize renderer");
    auto cl=device->createCommandList();cl->open();g.UploadDummies(cl);
    kknr::TextureFetchDesc desc;desc.width=desc.height=8;desc.base_address=0x1000;
    desc.format=kknr::TextureFormat::k_8_8_8_8;desc.max_level=0;desc.tiled=false;
    auto fetch=kknr::MakeTextureFetch(desc);
    auto* entry=g.textures_.Bind(fetch,physical,0).entry;
    Check(entry!=nullptr,"create original cache entry");
    auto* original=g.UploadTexture(cl,*entry,fetch);Check(original!=nullptr,"upload original");
    const auto original_index=g.BindTexture(cl,fetch.words,kkshaders::TextureDimension::Tex2D,false);
    Check(original_index!=0,"original descriptor");
    cl->close();host->Execute(cl);host->WaitIdle();
    const auto before=host->ReadBack(original->texture);
    auto folder=std::filesystem::current_path()/"texture-pack-gpu-fixtures"/
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(folder);
    kknr::HostTextureData replacement;replacement.plan.format=kknr::HostFormat::RGBA8_UNORM;
    replacement.plan.width=1025;replacement.plan.height=513;replacement.plan.levels=11;
    for(unsigned l=0;l<replacement.plan.levels;++l) {
      unsigned w=std::max(1u,replacement.plan.width>>l),h=std::max(1u,replacement.plan.height>>l);
      auto offset=replacement.bytes.size();replacement.subresources.push_back({l,0,w,h,1,w*4,w*h*4,offset,w*h*4});
      replacement.bytes.resize(offset+size_t(w)*h*4);
      for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
        auto* p=replacement.bytes.data()+offset+(size_t(y)*w+x)*4;
        p[0]=uint8_t(x);p[1]=uint8_t(y);p[2]=uint8_t(l*19);p[3]=uint8_t((x+y)%256);
      }
    }
    kknr::TexturePackEntry e{original->pack_hash,"bf_12345678_0000000C","",original->plan.format,8,8};
    e.file=kknr::TexturePackKey(e.hash,e.name)+".dds";
    Check(kknr_tools::WriteDds((folder/e.file).string(),replacement),"write synthetic DDS");
    kknr::TexturePackManifest m;m.entries.push_back(e);Check(m.Write(folder),"write manifest");
    g.options_.texture_pack_folder=folder.string();g.options_.texture_pack=true;
    unsigned frames=0;const auto start=std::chrono::steady_clock::now();
    while((!original->replacement || !original->replacement->pack_ready) && frames<1000) {
      ++g.frame_;cl->open();g.ServiceTexturePacks(cl);
      if(original->replacement && !original->replacement->pack_ready)
        Check(g.BindTexture(cl,fetch.words,kkshaders::TextureDimension::Tex2D,false)==original_index,"incomplete upload uses original");
      cl->close();host->Execute(cl);host->WaitIdle();++frames;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const double ready_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    Check(original->replacement && original->replacement->pack_ready,"async pack becomes ready");
    Check(frames>=replacement.plan.levels+1,"large base staged over multiple frames");
    const auto pixels=host->ReadBack(original->replacement->texture);
    Check(pixels.width==1025 && pixels.height==513,"NPOT replacement dimensions");
    for(unsigned y=0;y<pixels.height;++y)for(unsigned x=0;x<pixels.width;++x) {
      const auto* actual=pixels.at(x,y);const auto* expected=replacement.bytes.data()+(size_t(y)*pixels.width+x)*4;
      for(unsigned c=0;c<4;++c)Check(std::abs(actual[c]-expected[c]/255.f)<0.00001f,"exact strip pixel and alpha");
    }
    cl->open();const auto pack_index=g.BindTexture(cl,fetch.words,kkshaders::TextureDimension::Tex2D,false);
    Check(pack_index && pack_index!=original_index,"On selects pack descriptor");
    g.options_.texture_pack=false;
    Check(g.BindTexture(cl,fetch.words,kkshaders::TextureDimension::Tex2D,false)==original_index,"Off immediately restores original");
    g.options_.texture_pack=true;
    Check(g.BindTexture(cl,fetch.words,kkshaders::TextureDimension::Tex2D,false)==pack_index,"On reuses pack without reload");
    cl->close();host->Execute(cl);host->WaitIdle();
    const auto after=host->ReadBack(original->texture);Check(before.rgba==after.rgba,"original pixels retained");
    const auto bytes=g.pack_bytes_;
    g.options_.memory_budget=[] { return MemoryBudget{1ull<<30,1ull<<30,true}; };
    g.memory_sample_={};g.CheckMemoryBudget();
    Check(g.pack_pressure_ && !original->replacement && !g.pack_bytes_,"memory pressure drops pack");
    cl->open();Check(g.BindTexture(cl,fetch.words,kkshaders::TextureDimension::Tex2D,false)==original_index,"pressure uses original descriptor");
    cl->close();host->Execute(cl);host->WaitIdle();
    Check(host->errors()==0,"API and NVRHI validation");
    std::printf("PASS texture pack: %u service frames, %.2f ms until ready, %.2f ms total test, %llu resident bytes, zero validation errors\n",frames,
        ready_ms,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(),(unsigned long long)bytes);
  }
};
}
int main(int argc,char** argv) {
#if NR_HAS_D3D12
  if(argc>1 && std::string(argv[1])=="--d3d12") { auto host=nr::test::MemoryD3D12TestDevice::Create();if(!host)return 2;nr::TexturePackTestAccess::Run(host);return 0; }
#endif
  auto host=nr::test::VulkanTestDevice::Create(true);if(!host)return 2;nr::TexturePackTestAccess::Run(host);return 0;
}
