#include "test.h"
#include "kknr/texture_pack.h"
#include "../tools/image_io.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
using namespace kknr;
namespace fs=std::filesystem;
namespace {
fs::path Fixture() {
  static unsigned id=0;
  auto p=fs::current_path()/"texture-pack-fixtures"/
      (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++id));
  fs::create_directories(p); return p;
}
TexturePackEntry Entry(HostFormat format=HostFormat::RGBA8_UNORM) {
  TexturePackEntry e{0x123456789abcdef0ull,"bf_12345678_0000000C","",format,4,4};
  e.file=TexturePackKey(e.hash,e.name)+".dds"; return e;
}
HostTextureData Image(HostFormat f,uint32_t w,uint32_t h,uint32_t levels=1) {
  HostTextureData t; t.plan.format=f; t.plan.width=w; t.plan.height=h; t.plan.levels=levels;
  const auto& info=GetHostFormatInfo(f);
  for(uint32_t l=0;l<levels;++l) {
    const uint32_t lw=std::max(1u,w>>l),lh=std::max(1u,h>>l);
    const uint32_t row=((lw+info.block_size-1)/info.block_size)*info.bytes_per_block;
    const uint32_t size=row*((lh+info.block_size-1)/info.block_size);
    t.subresources.push_back({l,0,lw,lh,1,row,size,t.bytes.size(),size});
    t.bytes.resize(t.bytes.size()+size,127);
  }
  return t;
}
void Store(const fs::path& p,const TexturePackEntry& e,const HostTextureData& image) {
  CHECK(kknr_tools::WriteDds((p/e.file).string(),image));
  TexturePackManifest m; m.entries.push_back(e); CHECK(m.Write(p));
}
std::shared_ptr<const HostTextureData> Wait(TexturePackStream& stream,const TexturePackEntry& e) {
  HostTexturePlan p; p.format=e.format;p.width=e.width;p.height=e.height;
  for(unsigned i=0;i<1000;++i) {
    if(auto data=stream.Take(e.hash,p)) return data;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return {};
}
void Word(std::vector<uint8_t>& b,size_t at,uint32_t value) {
  for(int c=0;c<4;++c)b[at+c]=uint8_t(value>>(c*8));
}
std::vector<uint8_t> Bytes(const fs::path& p) {
  std::ifstream f(p,std::ios::binary);return {(std::istreambuf_iterator<char>(f)),{}};
}
void Bytes(const fs::path& p,const std::vector<uint8_t>& b) {
  std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),b.size());
}
}
TEST(texture_pack_identity_is_base_only_and_raw_channels) {
  auto a=Image(HostFormat::RGBA8_UNORM,4,4,3);
  const auto hash=TextureBaseHash(a); CHECK(hash!=0);
  a.bytes.back()^=0xff; CHECK_EQ(hash,TextureBaseHash(a));
  a.plan.view_swizzle=MakeSwizzle(2,1,0,3); CHECK_EQ(hash,TextureBaseHash(a));
  a.bytes[0]^=1; CHECK(hash!=TextureBaseHash(a));
  a.plan.min_level=1; CHECK_EQ(TextureBaseHash(a),0ull);
  a.plan.min_level=0;a.plan.layers=6;CHECK_EQ(TextureBaseHash(a),0ull);
  CHECK(TexturePackKey(0x1234,"bf_12345678_0000000C")=="0000000000001234__bf_12345678_0000000C");
}
TEST(texture_pack_manifest_version_traversal_and_ambiguity) {
  const auto root=Fixture();auto e=Entry();Store(root,e,Image(e.format,4,4));
  TexturePackManifest m;std::string why;CHECK(m.Read(root,why));CHECK_EQ(m.entries.size(),1u);
  auto p=Image(e.format,4,4).plan;CHECK(m.entries[0].Matches(p));p.width=8;CHECK(!m.entries[0].Matches(p));
  e.file="../escape.dds";m.entries={e};CHECK(m.Write(root));CHECK(!m.Read(root,why));
  e=Entry();auto alias=e;alias.name="bf_00000000_0000000C";alias.file=TexturePackKey(alias.hash,alias.name)+".dds";
  m.entries={e,alias};CHECK(m.Write(root));CHECK(!m.Read(root,why));
  std::ofstream(root/"pack.txt")<<"KKTP 2\n";CHECK(!m.Read(root,why));
  std::ofstream(root/"pack.txt")<<"KKTP 1\nFFFFFFFFFFFFFFFF bf_12345678_0000000C BC3_UNORM 0 4 invalid.dds\n";
  CHECK(!m.Read(root,why));
}
TEST(texture_pack_dds_families_and_complete_mips) {
  const auto root=Fixture();std::string why;HostTextureData result;
  for(auto f:{HostFormat::BC1_UNORM,HostFormat::BC3_UNORM,HostFormat::BC5_UNORM,HostFormat::BC5_SNORM,HostFormat::BC7_UNORM}) {
    auto e=Entry(f==HostFormat::BC7_UNORM?HostFormat::BC3_UNORM:f);const auto src=Image(f,8,8,4);
    Store(root,e,src);CHECK(LoadTextureReplacement(root,e,result,why));CHECK(result.bytes==src.bytes);CHECK_EQ(result.plan.levels,4u);
  }
  auto e=Entry(HostFormat::BC3_UNORM);Store(root,e,Image(HostFormat::BC1_UNORM,8,8,4));
  CHECK(!LoadTextureReplacement(root,e,result,why)); // BC1 cannot preserve smooth alpha.
  e=Entry(HostFormat::BC5_UNORM);Store(root,e,Image(HostFormat::BC3_UNORM,8,8,4));CHECK(!LoadTextureReplacement(root,e,result,why));
  Store(root,e,Image(HostFormat::BC5_SNORM,8,8,4));CHECK(!LoadTextureReplacement(root,e,result,why));
  e=Entry(HostFormat::BC3_UNORM);Store(root,e,Image(HostFormat::BC7_UNORM,8,8,1));CHECK(!LoadTextureReplacement(root,e,result,why));
}
TEST(texture_pack_dds_missing_mips_and_bad_headers) {
  const auto root=Fixture();auto e=Entry(HostFormat::BC3_UNORM);std::string why;HostTextureData result;
  Store(root,e,Image(HostFormat::BC3_UNORM,8,8));CHECK(LoadTextureReplacement(root,e,result,why));
  CHECK_EQ(result.plan.format,HostFormat::RGBA8_UNORM);CHECK_EQ(result.plan.levels,4u);
  Store(root,e,Image(HostFormat::BC3_UNORM,8,8,4));const auto valid=Bytes(root/e.file);
  for(auto change:std::vector<std::pair<size_t,uint32_t>>{{4,123},{16,0xffffffff},{28,99},{128,78},{132,4},{136,4},{140,6},{144,2}}) {
    auto b=valid;Word(b,change.first,change.second);Bytes(root/e.file,b);CHECK(!LoadTextureReplacement(root,e,result,why));
  }
  auto b=valid;b.pop_back();Bytes(root/e.file,b);CHECK(!LoadTextureReplacement(root,e,result,why));
  b=valid;b.push_back(0);Bytes(root/e.file,b);CHECK(!LoadTextureReplacement(root,e,result,why));
}
#ifdef _WIN32
TEST(texture_pack_png_npot_alpha_and_normal_mips) {
  const auto root=Fixture();auto e=Entry();e.file=TexturePackKey(e.hash,e.name)+".png";
  std::vector<uint8_t> rgba(3*5*4,0);
  for(size_t i=0;i<15;++i) { rgba[i*4]=20;rgba[i*4+1]=60;rgba[i*4+2]=90;rgba[i*4+3]=uint8_t(i*10); }
  CHECK(kknr_tools::WritePng((root/e.file).string(),3,5,rgba));
  HostTextureData out;std::string why;CHECK(LoadTextureReplacement(root,e,out,why));
  CHECK_EQ(out.plan.width,3u);CHECK_EQ(out.plan.height,5u);CHECK_EQ(out.plan.levels,3u);
  CHECK_EQ(out.bytes[3],0u);CHECK_EQ(out.bytes[59],140u);CHECK_EQ(out.subresources[0].size,60u);
  const auto alpha_png=Bytes(root/e.file);
  auto no_alpha=alpha_png; no_alpha[25]=2;
  Bytes(root/e.file,no_alpha);CHECK(!LoadTextureReplacement(root,e,out,why));
  CHECK(why=="colour PNG requires an explicit alpha channel");
  Bytes(root/e.file,alpha_png);
  e.format=HostFormat::BC5_UNORM;CHECK(LoadTextureReplacement(root,e,out,why));CHECK_EQ(out.plan.format,HostFormat::RG8_UNORM);
  CHECK_EQ(out.bytes[0],20u);CHECK_EQ(out.bytes[1],60u);CHECK_EQ(out.subresources[0].size,30u);
  e.format=HostFormat::BC5_SNORM;CHECK(LoadTextureReplacement(root,e,out,why));CHECK_EQ(out.plan.format,HostFormat::RG8_SNORM);
  CHECK(int8_t(out.bytes[0])<0);CHECK(int8_t(out.bytes[1])<0);
}
#endif
TEST(texture_pack_stream_async_budget_and_generation) {
  const auto root=Fixture(),other=Fixture();auto e=Entry();auto src=Image(e.format,8,8,4);Store(root,e,src);
  TexturePackStream stream(2);stream.Configure(root.string());HostTexturePlan p;p.format=e.format;p.width=e.width;p.height=e.height;
  stream.Request(e.hash,p);auto result=Wait(stream,e);CHECK(bool(result));if(result)CHECK(result->bytes==src.bytes);
  stream.Request(e.hash,p);
  for(unsigned i=0;i<100;++i) { CHECK(!stream.Take(e.hash,p,1));std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
  result=Wait(stream,e);CHECK(bool(result));
  stream.Configure(other.string());stream.Request(e.hash,p);
  std::this_thread::sleep_for(std::chrono::milliseconds(40));CHECK(!stream.Take(e.hash,p));
  stream.Configure(root.string());stream.Request(e.hash,p);CHECK(bool(Wait(stream,e)));
  stream.Configure("");CHECK(!stream.Take(e.hash,p));
}
