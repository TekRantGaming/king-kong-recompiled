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
#include <cstring>
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
    unsigned frames=0,incomplete=0;const auto start=std::chrono::steady_clock::now();
    while((!original->replacement || !original->replacement->pack_ready) && frames<1000) {
      ++g.frame_;cl->open();g.ServiceTexturePacks(cl);
      if(original->replacement && !original->replacement->pack_ready) {
        ++incomplete;
        Check(g.BindTexture(cl,fetch.words,kkshaders::TextureDimension::Tex2D,false)==original_index,"incomplete upload uses original");
      }
      cl->close();host->Execute(cl);host->WaitIdle();++frames;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const double ready_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    Check(original->replacement && original->replacement->pack_ready,"async pack becomes ready");
    // The 2.1 MB base needs two 2 MiB gameplay strips; the ten smaller levels
    // share the second frame's budget instead of taking a frame each.
    Check(incomplete>=1,"large base staged over multiple frames");
    Check(incomplete<=2,"small levels share a frame's upload budget");
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
    // A 12 GiB budget allows a quarter (no fixed 512 MiB cap any more).
    g.options_.memory_budget=[] { return MemoryBudget{12ull<<30,2ull<<30,true}; };
    g.memory_sample_={};g.CheckMemoryBudget();
    Check(!g.pack_pressure_ && g.pack_limit_==3ull<<30,"pack limit is a quarter of the budget");
    // Over the limit but not under pressure: the original stays, the decoded
    // data is dropped and the file is not read again every frame.
    g.options_.memory_budget=[] { return MemoryBudget{1ull<<30,(1ull<<30)-(1ull<<30)/10-(17ull<<20),true}; };
    g.memory_sample_={};g.CheckMemoryBudget();
    Check(!g.pack_pressure_ && g.pack_limit_==1ull<<20,"small free space limits the pack");
    for(unsigned i=0;i<1000 && !original->pack_capped;++i) {
      ++g.frame_;cl->open();g.ServiceTexturePacks(cl);cl->close();host->Execute(cl);host->WaitIdle();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Check(original->pack_capped && !original->replacement && g.pack_capped_==1 && !g.pack_bytes_,"over-limit replacement keeps original");
    for(unsigned i=0;i<20;++i) { ++g.frame_;cl->open();g.ServiceTexturePacks(cl);cl->close();host->Execute(cl);host->WaitIdle(); }
    Check(original->pack_capped && g.pack_capped_==1,"capped original is not requested again");
    cl->open();Check(g.BindTexture(cl,fetch.words,kkshaders::TextureDimension::Tex2D,false)==original_index,"capped uses original descriptor");
    cl->close();host->Execute(cl);host->WaitIdle();
    // When room returns, the capped original retries once (after 600 frames).
    g.options_.memory_budget=[] { return MemoryBudget{12ull<<30,2ull<<30,true}; };
    g.memory_sample_={};g.CheckMemoryBudget();
    g.frame_+=600;
    for(unsigned i=0;i<1000 && !(original->replacement && original->replacement->pack_ready);++i) {
      ++g.frame_;cl->open();g.ServiceTexturePacks(cl);cl->close();host->Execute(cl);host->WaitIdle();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Check(original->replacement && original->replacement->pack_ready && !g.pack_capped_ && g.pack_bytes_==bytes,"capped original retries when room returns");
    // Task 180, sharing: a second original with the same pack identity (same
    // content elsewhere in memory) uses the same GPU replacement, not a copy.
    auto* physical_bytes=const_cast<uint8_t*>(guest.PhysicalBase());
    std::memcpy(physical_bytes+0x2000,physical_bytes+0x1000,0x1000);
    std::memset(physical_bytes+0x3000,0x5A,0x1000);
    auto fetch_at=[&](uint32_t address) { auto d=desc;d.base_address=address;return kknr::MakeTextureFetch(d); };
    const auto fetch2=fetch_at(0x2000),fetch3=fetch_at(0x3000);
    cl->open();
    auto* entry2=g.textures_.Bind(fetch2,physical,g.frame_).entry;auto* entry3=g.textures_.Bind(fetch3,physical,g.frame_).entry;
    Check(entry2 && entry3,"more original cache entries");
    auto* original2=g.UploadTexture(cl,*entry2,fetch2);auto* original3=g.UploadTexture(cl,*entry3,fetch3);
    Check(original2 && original3,"upload more originals");
    cl->close();host->Execute(cl);host->WaitIdle();
    Check(original2->pack_hash==original->pack_hash && original3->pack_hash && original3->pack_hash!=original->pack_hash,
        "same content, same identity; other content, other identity");
    kknr::TexturePackEntry e3{original3->pack_hash,"bf_12345678_0000000D","",original3->plan.format,8,8};
    e3.file=kknr::TexturePackKey(e3.hash,e3.name)+".dds";
    auto write_pack=[&](const char* name) {
      auto dir=folder/name;std::filesystem::create_directories(dir);
      Check(kknr_tools::WriteDds((dir/e.file).string(),replacement) && kknr_tools::WriteDds((dir/e3.file).string(),replacement),"write DDS");
      kknr::TexturePackManifest pack;pack.entries={e,e3};Check(pack.Write(dir),"write two-entry manifest");
      g.options_.texture_pack_folder=dir.string();
    };
    auto serve=[&](unsigned max_frames,auto&& done,auto&& each) {
      unsigned n=0;
      for(;n<max_frames && !done();++n) {
        ++g.frame_;cl->open();each();g.ServiceTexturePacks(cl);cl->close();host->Execute(cl);host->WaitIdle();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      return n;
    };
    auto ready=[](GameRenderer::HostTexture* o) { return o->replacement && o->replacement->pack_ready; };
    write_pack("shared");
    serve(2000,[&] { return ready(original) && ready(original2) && ready(original3); },[] {});
    Check(ready(original) && ready(original2) && ready(original3),"three originals get replacements");
    Check(original->replacement==original2->replacement && original->replacement->pack_refs==2,"equal identities share one replacement");
    Check(original3->replacement!=original->replacement && g.pack_shared_.size()==2 && g.pack_bytes_==2*bytes,
        "two identities, two allocations (no duplicate)");
    cl->open();
    const auto shared_index=g.BindTexture(cl,fetch.words,kkshaders::TextureDimension::Tex2D,false);
    Check(shared_index && g.BindTexture(cl,fetch2.words,kkshaders::TextureDimension::Tex2D,false)==shared_index,
        "both originals bind the shared replacement's descriptor");
    cl->close();host->Execute(cl);host->WaitIdle();
    // The texture cache drops one original: the other keeps the replacement.
    g.ReleaseHostTexture(original2);entry2->host=nullptr;
    Check(ready(original) && original->replacement->pack_refs==1 && g.pack_bytes_==2*bytes,"shared replacement outlives one original");
    // Task 180, chapter change: room for one replacement. The one that loads is the
    // old chapter's, the capped one the new chapter's.
    write_pack("chapter");
    g.pack_limit_=bytes+bytes/2;
    serve(2000,[&] { return (ready(original) || ready(original3)) && g.pack_capped_==1; },[] {});
    auto* old_chapter=ready(original) ? original : original3;
    auto* new_chapter=old_chapter==original ? original3 : original;
    const auto& new_fetch=new_chapter==original ? fetch : fetch3;
    const auto& old_fetch=old_chapter==original ? fetch : fetch3;
    Check(ready(old_chapter) && new_chapter->pack_capped && g.pack_capped_==1 && g.pack_bytes_==bytes,"limit holds one, other capped");
    ++g.frame_;g.TexturePackLoadStarted();
    auto bind_new=[&] { g.BindTexture(cl,new_fetch.words,kkshaders::TextureDimension::Tex2D,false); };
    serve(10,[] { return false; },bind_new);  // loading frames: only the new chapter's texture is used
    Check(ready(old_chapter) && new_chapter->pack_capped,"nothing freed while loading");
    g.TexturePackLoadEnded();
    const auto end_frame=g.frame_;
    serve(1000,[&] { return ready(new_chapter); },bind_new);
    Check(!old_chapter->replacement && old_chapter->pack_swept && g.pack_shared_.size()==1,"unused old replacement freed after the load");
    Check(ready(new_chapter) && !g.pack_capped_ && g.pack_bytes_==bytes,"capped new-chapter replacement loads once room is freed");
    const auto retry_frames=g.frame_-end_frame;
    Check(retry_frames<GameRenderer::kPackSweepDelayFrames+200,"capped retry is prompt (not the 600-frame wait)");
    serve(20,[] { return false; },[] {});
    Check(!old_chapter->replacement && old_chapter->pack_swept,"freed one is not requested again while unused");
    // Needed again: it reloads normally.
    g.pack_limit_=3ull<<30;
    serve(1000,[&] { return ready(old_chapter); },[&] { g.BindTexture(cl,old_fetch.words,kkshaders::TextureDimension::Tex2D,false); });
    Check(ready(old_chapter) && !old_chapter->pack_swept && g.pack_bytes_==2*bytes,"freed replacement reloads when used again");
    // Stream Cancel (used for parked/released originals): no ready bytes are left, and it can be requested again.
    auto& stream=*g.texture_pack_stream_;
    stream.Request(original3->pack_hash,original3->plan);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // ready, or still being read (dropped then too)
    stream.Cancel(original3->pack_hash,original3->plan);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    Check(!stream.Take(original3->pack_hash,original3->plan),"cancelled identity leaves no ready bytes");
    stream.Request(original3->pack_hash,original3->plan);
    std::shared_ptr<const kknr::HostTextureData> again;
    for(unsigned i=0;i<1000 && !again;++i) {
      again=stream.Take(original3->pack_hash,original3->plan);std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Check(bool(again),"cancelled identity can be requested again");
    Check(host->errors()==0,"API and NVRHI validation");
    std::printf("PASS texture pack: %u service frames, %.2f ms until ready, %.2f ms total test, %llu resident bytes, shared copy, "
        "chapter-change retry after %llu frames, zero validation errors\n",frames,
        ready_ms,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(),(unsigned long long)bytes,
        (unsigned long long)retry_frames);
  }
};
}
int main(int argc,char** argv) {
#if NR_HAS_D3D12
  if(argc>1 && std::string(argv[1])=="--d3d12") { auto host=nr::test::MemoryD3D12TestDevice::Create();if(!host)return 2;nr::TexturePackTestAccess::Run(host);return 0; }
#endif
  auto host=nr::test::VulkanTestDevice::Create(true);if(!host)return 2;nr::TexturePackTestAccess::Run(host);return 0;
}
