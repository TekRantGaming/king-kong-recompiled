// Task 124's narrow selector experiment, developer builds only. No player setting.
// It changes only the selector's output pointer, never the importance/threshold bytes.
#if defined(KK_DEV_TOOLS)
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <map>
#include <set>
#include <tuple>
#include <rex/hook.h>
namespace kk { double DevSecondsSinceMenu(); }
namespace {
uint32_t U32(const uint8_t* b, uint32_t a) {
  uint32_t v; std::memcpy(&v,b+a,4); return std::byteswap(v);
}
void W32(uint8_t* b,uint32_t a,uint32_t v) { v=std::byteswap(v);std::memcpy(b+a,&v,4); }
struct Fields {
  std::array<uint8_t,32> object{};
  uint16_t descriptor_flags=0;
  std::array<uint8_t,3> ai{};
  bool operator==(const Fields&) const = default;
};
Fields Read(const uint8_t* b,uint32_t object) {
  Fields f; std::memcpy(f.object.data(),b+object,32);
  const auto descriptor=U32(b,object+24),holder=U32(b,object+20);
  if(descriptor) std::memcpy(&f.descriptor_flags,b+descriptor+46,2);
  const auto associated=holder?U32(b,holder):0;
  if(associated) std::memcpy(f.ai.data(),b+associated+165,3);
  return f;
}
struct Probe {
  FILE* file=nullptr; FILE* draws=nullptr; unsigned bias=1;
  std::mutex mutex;
  std::set<std::tuple<uint32_t,uint32_t,uint32_t,uint32_t>> seen;
  std::map<uint32_t,uint32_t> selected;
  std::set<std::tuple<uint32_t,uint32_t,uint32_t>> draw_seen;
  Probe() {
    if(const char* p=std::getenv("KK_DEV_LOD_PROBE");p&&*p) {
      file=std::fopen(p,"w");
      const auto path=std::string(p)+".draw.csv"; draws=std::fopen(path.c_str(),"w");
      if(draws) std::fprintf(draws,"seconds,object,resource,selector_output,draw_child,same\n");
      if(file) std::fprintf(file,"seconds,caller,object,resource,count,importance,original_index,biased_index,original_child,biased_child,object_equal,flags_equal,ai_equal,importance_after,activation_before,activation_after,ai165_before,ai166_before,ai167_before,ai165_after,ai166_after,ai167_after,applied,selector_result\n");
    }
    if(const char* v=std::getenv("KK_DEV_LOD_BIAS")) bias=std::clamp(std::atoi(v),1,4);
  }
  ~Probe(){if(file)std::fclose(file);if(draws)std::fclose(draws);}
};
Probe& Get(){static Probe p;return p;}
}
REX_EXTERN(__imp__sub_827D2080);
REX_HOOK_RAW(sub_827D2080) {
  auto& p=Get();
  if(!p.file||kk::DevSecondsSinceMenu()<0) {__imp__sub_827D2080(ctx,base);return;}
  const auto wrapper=ctx.r3.u32,out=ctx.r4.u32,caller=uint32_t(ctx.lr);
  const auto gdi=U32(base,0x82CCAC30),object=gdi?U32(base,gdi+156):0;
  if(!object||!wrapper||!out) {__imp__sub_827D2080(ctx,base);return;}
  const auto resource=U32(base,wrapper); const unsigned importance=base[object+28];
  const auto before=Read(base,object);
  __imp__sub_827D2080(ctx,base);
  const auto original=U32(base,out),count=unsigned(base[resource+8]);
  unsigned old_index=0,new_index=0;
  if(count>0&&count<=6) {
    const unsigned value=std::min(255u,unsigned(importance)*p.bias);
    while(old_index+1<count&&base[resource+10+old_index]>=importance)++old_index;
    while(new_index+1<count&&base[resource+10+new_index]>=value)++new_index;
  }
  const uint32_t child=count>0&&count<=6?U32(base,resource+16+new_index*4):original;
  // Restrict the experiment to the two statically inspected rendering callers.
  const bool applied=(caller==0x82877044||caller==0x82876D70)&&child&&ctx.r3.u32;
  if(applied) W32(base,out,child);
  const auto after=Read(base,object);
  std::lock_guard lock(p.mutex);
  if(applied) p.selected[object]=child;
  if(p.seen.size()<20000&&p.seen.emplace(object,resource,old_index,new_index).second) {
    std::fprintf(p.file,"%.6f,%08X,%08X,%08X,%u,%u,%u,%u,%08X,%08X,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
      kk::DevSecondsSinceMenu(),caller,object,resource,count,importance,old_index,new_index,original,child,
      before.object==after.object,before.descriptor_flags==after.descriptor_flags,before.ai==after.ai,
      after.object[28],before.object[31],after.object[31],before.ai[0],before.ai[1],before.ai[2],after.ai[0],after.ai[1],after.ai[2],applied,ctx.r3.u32);
    std::fflush(p.file);
  }
}
REX_EXTERN(__imp__sub_827D2028);
REX_HOOK_RAW(sub_827D2028) {
  auto& p=Get();
  if(p.draws&&kk::DevSecondsSinceMenu()>=0) {
    const auto object=ctx.r3.u32,holder=U32(base,object+20),wrapper=U32(base,holder),resource=U32(base,wrapper);
    const auto drawn=U32(base,resource+16);
    std::lock_guard lock(p.mutex);
    if(auto it=p.selected.find(object);it!=p.selected.end()&&p.draw_seen.size()<20000&&
       p.draw_seen.emplace(object,it->second,drawn).second) {
      std::fprintf(p.draws,"%.6f,%08X,%08X,%08X,%08X,%u\n",kk::DevSecondsSinceMenu(),object,resource,it->second,drawn,it->second==drawn);
      std::fflush(p.draws);
    }
  }
  __imp__sub_827D2028(ctx,base);
}
#endif
