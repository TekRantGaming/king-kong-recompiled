// C133: the saved 28f hang is a raptor grid walk with NaN coordinates, not
// an established driver stall. Its two loop limits became INT_MAX. Refuse
// an invalid line before conversion; the native return value 0 means blocked.
#include "coop_grid_guard.h"
#include "coop_raptor_route.h"
#include "hfr.h"
#include "coop_pose.h"
#include "coop_stimulus_debug.h"
#include "coop_guest_probe.h"
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DECLARE(bool, kk_coop_enabled);
REX_EXTERN(__imp__sub_82556890);
REX_EXTERN(__imp__sub_825924A8);
REX_EXTERN(__imp__sub_825757B0);
REX_EXTERN(__imp__sub_82572298);
REX_EXTERN(__imp__sub_824759C0);
REX_EXTERN(__imp__sub_82465920);
REX_EXTERN(__imp__sub_826E9220);
REX_EXTERN(__imp__sub_826D5CD8);
REX_EXTERN(__imp__sub_82593B98);
REX_EXTERN(__imp__sub_825E2178);
REX_EXTERN(__imp__sub_825B7BA8);
REX_EXTERN(__imp__sub_827117E0);
REX_EXTERN(__imp__sub_82711718);
REX_EXTERN(__imp__sub_82710E20);
REX_EXTERN(__imp__sub_82409B80);
REX_EXTERN(__imp__sub_82447DB0);
REX_EXTERN(__imp__sub_8285AB30);
REX_EXTERN(__imp__sub_8285ADE8);
REX_EXTERN(__imp__sub_82491B50);
REX_EXTERN(__imp__sub_8245B0D0);
REX_EXTERN(__imp__sub_8261D338);
#if defined(KK_DEV_TOOLS)
namespace kk { double DevSecondsSinceMenu(); }
#endif

namespace kk::coop::raptor_route {
using namespace kk::hfr;
bool Readable(uint32_t p,uint32_t bytes) {
  if (!PoseAddress(p,bytes)) return false;
  auto* memory=rex::system::kernel_memory(); if(!memory) return false;
  return guest_probe::Readable(p,bytes,[&](uint32_t a,guest_probe::Region& r) {
    auto* heap=memory->LookupHeap(a); if(!heap || !heap->page_size()) return false;
    r.base=a & ~(heap->page_size()-1);r.size=heap->page_size();
    rex::memory::HeapAllocationInfo info{};
    if(!heap->QueryRegionInfo(r.base,&info)) return false;
    r.readable=(info.state & rex::memory::kMemoryAllocationCommit) &&
        (info.protect & rex::memory::kMemoryProtectRead) && info.region_size;
    return true;
  });
}
struct Owner { uint32_t actor=0, ai=0, vars=0, matrix=0; };
thread_local Owner current;
thread_local uint64_t grid_refusals=0, recovered_grid=0;
thread_local Owner refused_grid, refused_orientation, refused_curve;
bool Same(Owner a,Owner b) { return a.vars && a.actor==b.actor && a.ai==b.ai && a.vars==b.vars && a.matrix==b.matrix; }
bool PendingGrid() { return grid_refusals!=recovered_grid && Same(current,refused_grid); }
bool PendingOrientation() { return Same(current,refused_orientation); }
bool PendingCurve() { return Same(current,refused_curve); }
struct ResumeWatch { Owner owner; Point3 from; bool pending=false; };
ResumeWatch watch;
Point3 ReadPoint(const uint8_t* b,uint32_t p) { return {LoadF(b,p),LoadF(b,p+4),LoadF(b,p+8)}; }
struct Scope {
  Owner saved=current;
  explicit Scope(uint8_t* b) {
    current={};
    if (!REXCVAR_GET(kk_coop_enabled)) return;
    if (!saved.vars) {refused_grid={};refused_orientation={};refused_curve={};recovered_grid=grid_refusals;}
    const auto actor=Load32(b,0x82CCBF6C), ai=Load32(b,0x82CCBF64);
    if (!Readable(actor,28) || !Readable(ai,68)) return;
    const auto vars=Load32(b,ai+64), matrix=Load32(b,actor+12);
    if (Readable(vars,14240) && Readable(matrix,64)) current={actor,ai,vars,matrix};
  }
  ~Scope() { current=saved; }
};
bool Finite(const uint8_t* b,uint32_t p) {
  return std::isfinite(LoadF(b,p)) && std::isfinite(LoadF(b,p+4)) && std::isfinite(LoadF(b,p+8));
}
stimulus_debug::Record Event(const uint8_t* b,uint32_t handle) {
  return stimulus_debug::ReadHandle([&](uint32_t p){return Readable(p,4) ? Load32(b,p):0u;},handle);
}
void Point(const uint8_t* b,uint32_t p,const char* source,uint32_t caller) {
  const bool owned=(p>=current.vars && uint64_t(p)+12<=uint64_t(current.vars)+14240) ||
      (p>=current.matrix && uint64_t(p)+12<=uint64_t(current.matrix)+64);
  if (!current.vars || !PoseAddress(p,12) || (!owned && !Readable(p,12)) || Finite(b,p)) return;
  static grid_guard::Reports reports;
  struct Seen {Owner owner;uint32_t mask=0;};
  static std::array<Seen,64> seen{};
  static uint32_t world=0;
  const auto now_world=Load32(b,0x828E8388);
  if(world!=now_world) {world=now_world;seen={};}
  uint32_t bit=128;
  if(p==current.matrix+48) bit=1;
  else if(p==current.vars+2572) bit=2;
  else if(p==current.vars+2584) bit=4;
  else if(p>=current.vars+2652 && p<current.vars+3852) bit=8;
  else if(std::strcmp(source,"threat event")==0) bit=16;
  else if(std::strcmp(source,"follow position")==0) bit=32;
  else if(std::strcmp(source,"waypoint position")==0) bit=64;
  else if(std::strcmp(source,"native matrix output")==0) bit=256;
  else if(std::strcmp(source,"velocity setter input")==0) bit=512;
  else if(std::strcmp(source,"position setter input")==0) bit=1024;
  else if(std::strcmp(source,"selected target position")==0) bit=2048;
  else if(std::strcmp(source,"candidate position")==0) bit=4096;
  bool first=false;
  for(auto& entry:seen) if(!entry.owner.vars || Same(entry.owner,current)) {
    if(!entry.owner.vars) entry.owner=current;
    first=!(entry.mask&bit);entry.mask|=bit;break;
  }
  const double now=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if (!reports.Note(now) && !first) return;
  const auto v=current.vars;
  const auto event=Event(b,v+5216);
  REXLOG_WARN("KK co op raptor invalid: source={} actor={:08X} ai={:08X} vars={:08X} caller={:08X} "
      "point={:08X} value=({},{},{}) bits={:08X}/{:08X}/{:08X} action={} flags={:08X} "
      "event={:08X} target={:08X} follow={:08X} node={:08X} count={} refused={}",
      source,current.actor,current.ai,v,caller,p,LoadF(b,p),LoadF(b,p+4),LoadF(b,p+8),
      Load32(b,p),Load32(b,p+4),Load32(b,p+8),Load32(b,v+6756),Load32(b,v+6732),
      event.id,event.target,Load32(b,v+7148),Load32(b,v+7152),Load32(b,v+2644),reports.refused);
}
void Inputs(const uint8_t* b,const char* source,uint32_t caller) {
  if (!current.vars) return;
  // A native callback can destroy its actor. Do not follow its old graph or
  // event pointers after return, even if the freed allocation stays mapped.
  if (!Readable(current.actor,28) || !Readable(current.ai,68) ||
      Load32(b,current.actor+12)!=current.matrix || Load32(b,current.ai+64)!=current.vars ||
      !Readable(current.vars,14240) || !Readable(current.matrix,64)) return;
  const auto ext=Load32(b,current.actor+24);
  if (!Readable(ext,8) || Load32(b,ext+4)!=current.ai) return;
  const auto v=current.vars;
  if (watch.pending && watch.owner.actor==current.actor && watch.owner.ai==current.ai &&
      watch.owner.vars==v && watch.owner.matrix==current.matrix && Finite(b,current.matrix+48)) {
    const auto p=ReadPoint(b,current.matrix+48);
    const auto travel=LoadF(b,v+5176);
    if (std::hypot(p[0]-watch.from[0],p[1]-watch.from[1])>=.25f && Load32(b,v+2556) && std::isfinite(travel) && travel>0) {
      REXLOG_INFO("KK co op raptor recovery: native movement actor={:08X} from=({},{},{}) to=({},{},{}) moving={} travel={} cells={}",
          current.actor,watch.from[0],watch.from[1],watch.from[2],p[0],p[1],p[2],Load32(b,v+2556),travel,Load32(b,v+2644));
      watch.pending=false;
    }
  }
  Point(b,current.matrix+48,"actor position",caller);
  Point(b,v+2572,source,caller);
  Point(b,v+2584,"retained step",caller);
  const auto event=Event(b,v+5216);
  if (event.valid) {
    Point(b,event.address+32,"threat event",caller);
    if(Readable(event.target,16)) {
      const auto matrix=Load32(b,event.target+12);
      if(Readable(matrix,64)) Point(b,matrix+48,"selected target position",caller);
    }
  }
  const auto candidates=Load32(b,v+5236);
  if(candidates<=20) for(uint32_t i=0;i<candidates;++i) Point(b,v+6000+12*i,"candidate position",caller);
  for (auto offset : {7148u,7152u}) {
    const auto object=Load32(b,v+offset);
    if (!Readable(object,16)) continue;
    const auto matrix=Load32(b,object+12);
    if (Readable(matrix,64)) Point(b,matrix+48,offset==7148 ? "follow position":"waypoint position",caller);
  }
  const auto count=Load32(b,v+2644);
  if (count<=100) for (uint32_t i=0;i<count;++i) Point(b,v+2652+12*i,"cached path",caller);
}
bool Recover(PPCContext& ctx,uint8_t* b,const char* reason) {
  const auto v=current.vars;
  if (!v || Load32(b,0x82CCBF64)!=current.ai || Load32(b,0x82CCBF6C)!=current.actor ||
      Load32(b,current.ai+64)!=v || Load32(b,current.actor+12)!=current.matrix || (Load32(b,current.actor)&1)) return false;
  const auto own=ReadPoint(b,current.matrix+48);
  const bool reset=ResetRoute([&](uint32_t p){return Load32(b,p);},
      [&](uint32_t p,uint32_t value){Store32(b,p,value);},
      [&](uint32_t p,float value){StoreF(b,p,value);},[&]{
        const PPCContext saved=ctx;
        __imp__sub_82447DB0(ctx,b);
        ctx=saved;
      },v,own);
  if (!reset) return false;
  recovered_grid=grid_refusals;
  if (PendingOrientation()) refused_orientation={};
  if (PendingCurve()) refused_curve={};
  watch={current,own,true};
  static grid_guard::Reports reports;
  const double now=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if (reports.Note(now)) REXLOG_WARN("KK co op raptor recovery: route cleared actor={:08X} ai={:08X} reason={} own=({},{},{}) count={}",
      current.actor,current.ai,reason,own[0],own[1],own[2],reports.refused);
  return true;
}
bool BadRoute(const uint8_t* b) {
  if (!current.vars) return false;
  const auto v=current.vars;
  if (!Finite(b,current.matrix+48) || !Finite(b,v+2572) || !Finite(b,v+2584)) return true;
  const auto count=int32_t(Load32(b,v+2644));
  if (count>100) return true;
  for (int32_t i=0;i<count;++i) if (!Finite(b,v+2652+12*i)) return true;
  return false;
}
#if defined(KK_DEV_TOOLS)
bool Force(const uint8_t* b,const char* mode) {
  static bool done=false;
  const char* value=std::getenv("KK_DEV_RAPTOR_NAN");
  if (done || !value || std::strcmp(value,mode)!=0 || !current.vars ||
      (std::strcmp(mode,"orientation")!=0 && Load32(b,current.vars+5108)!=2) ||
      !(LoadF(b,current.vars+20)>0) || kk::DevSecondsSinceMenu()<35) return false;
  done=true;
  REXLOG_WARN("KK dev raptor force: mode={} actor={:08X} ai={:08X}",mode,current.actor,current.ai);
  return true;
}
#endif
bool Raptor(const uint8_t* b) {
  const auto ai=Load32(b,0x82CCBF64);
  if (!PoseAddress(ai,88)) return false;
  const auto first=Load16(b,ai+80), last=Load16(b,ai+82);
  if (first>last || last>4096 || !PoseAddress(ai,4*(last+7))) return false;
  for (uint32_t t=first;t<=last;++t) {
    const auto block=Load32(b,ai+4*(t+6));
    if (!PoseAddress(block,24)) continue;
    const auto fn=Load32(b,block+20);
    if (fn==0x82572E40 || fn==0x8254BCB0 || fn==0x825B9020) return true;
  }
  return false;
}
void WriteInput(PPCContext& ctx,uint8_t* b,const char* source,bool pair=false) {
  if (!REXCVAR_GET(kk_coop_enabled) || !PoseAddress(ctx.r4.u32,12) ||
      (Finite(b,ctx.r4.u32) && (!pair || !PoseAddress(ctx.r5.u32,12) || Finite(b,ctx.r5.u32))) ||
      ctx.r3.u32!=Load32(b,0x82CCBF6C) || !Raptor(b)) return;
  Scope scope(b);
  Point(b,ctx.r4.u32,source,uint32_t(ctx.lr));
  if(pair) Point(b,ctx.r5.u32,source,uint32_t(ctx.lr));
}
void WriteOutput(uint8_t* b,uint32_t actor,uint32_t caller,const char* source) {
  if (!REXCVAR_GET(kk_coop_enabled) || actor!=Load32(b,0x82CCBF6C) || !PoseAddress(actor,16)) return;
  const auto m=Load32(b,actor+12);
  if (!PoseAddress(m,64) || (Finite(b,m) && Finite(b,m+16) && Finite(b,m+32) && Finite(b,m+48)) || !Raptor(b)) return;
  Scope scope(b);
  for (uint32_t off : {0u,16u,32u,48u}) Point(b,m+off,source,caller);
}
void Matrix(PPCContext& ctx,uint8_t* b,bool look_y,void (*native)(PPCContext&,uint8_t*)) {
  if (!REXCVAR_GET(kk_coop_enabled)) {native(ctx,b);return;}
  const auto actor=Load32(b,0x82CCBF6C);
  if (!PoseAddress(actor,16) || ctx.r3.u32!=Load32(b,actor+12)) {native(ctx,b);return;}
  Scope scope(b);
  if (!current.vars || !Raptor(b)) {native(ctx,b);return;}
  const auto caller=uint32_t(ctx.lr), matrix=ctx.r3.u32;
  const auto first=ctx.r4.u32 ? ctx.r4.u32 : matrix+16;
  const auto second=ctx.r5.u32 ? ctx.r5.u32 : matrix+32;
  if (!PoseAddress(first,12) || !PoseAddress(second,12)) {native(ctx,b);return;}
  // mat_look_z has optional up in r4 and mandatory direction in r5.
  // mat_look_y has mandatory direction in r4 and optional up in r5.
#if defined(KK_DEV_TOOLS)
  uint32_t injected=0, words[3]{};
  const auto direction=look_y ? ctx.r4.u32:ctx.r5.u32;
  if (direction>=ctx.r1.u32 && uint64_t(direction)+12<=uint64_t(ctx.r1.u32)+4096 && Force(b,"orientation")) {
    injected=direction;
    for(unsigned i=0;i<3;++i) {words[i]=Load32(b,injected+4*i);Store32(b,injected+4*i,0);}
  }
  const auto off=std::getenv("KK_DEV_RAPTOR_SOURCE_OFF");
  const bool enabled=!(off && std::strcmp(off,"1")==0);
#else
  constexpr bool enabled=true;
#endif
  const auto a=ReadPoint(b,first), c=ReadPoint(b,second);
  if (RefuseOrientation(enabled,true,a,c)) {
    static grid_guard::Reports reports;
    const double now=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (reports.Note(now)) REXLOG_WARN("KK co op raptor orientation refused: actor={:08X} caller={:08X} look_y={} first=({},{},{}) second=({},{},{}) count={}",
        current.actor,caller,look_y,a[0],a[1],a[2],c[0],c[1],c[2],reports.refused);
    // A caller may still follow waypoint pointers. Clear only after return.
    refused_orientation=current;
  } else native(ctx,b);
  for(uint32_t off:{0u,16u,32u,48u}) Point(b,matrix+off,"native matrix output",caller);
#if defined(KK_DEV_TOOLS)
  if(injected) for(unsigned i=0;i<3;++i) Store32(b,injected+4*i,words[i]);
#endif
}
bool CurveStop(PPCContext& ctx,uint8_t* b,uint32_t out,uint32_t tangent,const Curve& curve,uint32_t caller,const char* reason) {
  if(!current.vars || !Readable(out,12) || !Readable(tangent,12)) return false;
  if(Load32(b,current.actor)&1) return false; // Parent space needs the native world position resolver.
  if(uint64_t(out)<uint64_t(tangent)+12 && uint64_t(tangent)<uint64_t(out)+12) return false;
  const auto own=ReadPoint(b,current.matrix+48);
  if(!Finite(own)) return false;
  for(unsigned i=0;i<3;++i) {StoreF(b,out+4*i,own[i]);StoreF(b,tangent+4*i,0);}
  ctx.r3.u64=out;
  refused_curve=current;
  static grid_guard::Reports reports;
  const double now=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if(reports.Note(now)) REXLOG_WARN("KK co op raptor curve refused: actor={:08X} ai={:08X} caller={:08X} source={} parameter={} cells={} "
      "p0=({},{},{}) p1=({},{},{}) p2=({},{},{}) p3=({},{},{}) own=({},{},{}) count={}",
      current.actor,current.ai,caller,reason,curve.parameter,Load32(b,current.vars+2644),
      curve.points[0][0],curve.points[0][1],curve.points[0][2],curve.points[1][0],curve.points[1][1],curve.points[1][2],
      curve.points[2][0],curve.points[2][1],curve.points[2][2],curve.points[3][0],curve.points[3][1],curve.points[3][2],
      own[0],own[1],own[2],reports.refused);
  return true;
}
void Bezier(PPCContext& ctx,uint8_t* b) {
  const auto out=ctx.r3.u32,caller=uint32_t(ctx.lr);
  if(!current.vars || !MotionCurveCaller(caller) || !Readable(ctx.r1.u32+80,24)) {
    __imp__sub_8261D338(ctx,b);Point(b,out,"Bezier output",caller);return;
  }
  // f1 reserves GPR slot r4. Four packed vectors start at r5, r7, r9
  // and caller SP+80. The tangent pointer is the word at caller SP+100.
  const Curve curve{ctx.f1.f64,{PackedPoint(ctx.r5.u64,ctx.r6.u64),PackedPoint(ctx.r7.u64,ctx.r8.u64),
      PackedPoint(ctx.r9.u64,ctx.r10.u64),ReadPoint(b,ctx.r1.u32+80)}};
  const auto tangent=Load32(b,ctx.r1.u32+100);
#if defined(KK_DEV_TOOLS)
  const auto off=std::getenv("KK_DEV_RAPTOR_SOURCE_OFF");
  const bool enabled=!(off && std::strcmp(off,"1")==0);
#else
  constexpr bool enabled=true;
#endif
  if(RefuseCurve(enabled,true,caller,curve) && CurveStop(ctx,b,out,tangent,curve,caller,"native movement controls")) return;
  __imp__sub_8261D338(ctx,b);
  Point(b,out,"Bezier output",caller);
  // Finite controls can still overflow native arithmetic. Check its two
  // scratch outputs before the caller turns them into an actor velocity.
  if(enabled && Readable(out,12) && Readable(tangent,12) && (!Finite(b,out) || !Finite(b,tangent)))
    CurveStop(ctx,b,out,tangent,curve,caller,"native movement result");
}
} // namespace kk::coop::raptor_route

REX_HOOK_RAW(sub_82593B98) {
  kk::coop::raptor_route::Scope scope(base);
  kk::coop::raptor_route::Inputs(base,"blend input",uint32_t(ctx.lr));
  __imp__sub_82593B98(ctx,base);
  kk::coop::raptor_route::Inputs(base,"blend output",0x82593B98);
  if (kk::coop::raptor_route::PendingOrientation()) kk::coop::raptor_route::Recover(ctx,base,"orientation input");
  if (kk::coop::raptor_route::PendingCurve()) kk::coop::raptor_route::Recover(ctx,base,"invalid movement curve");
}
REX_HOOK_RAW(sub_825E2178) {
  kk::coop::raptor_route::Scope scope(base);
  kk::coop::raptor_route::Inputs(base,"fight input",uint32_t(ctx.lr));
  __imp__sub_825E2178(ctx,base);
  kk::coop::raptor_route::Inputs(base,"fight output",0x825E2178);
  if (kk::coop::raptor_route::PendingOrientation()) kk::coop::raptor_route::Recover(ctx,base,"orientation input");
}
REX_HOOK_RAW(sub_825B7BA8) {
  kk::coop::raptor_route::Scope scope(base);
  kk::coop::raptor_route::Inputs(base,"action input",uint32_t(ctx.lr));
  __imp__sub_825B7BA8(ctx,base);
  kk::coop::raptor_route::Inputs(base,"action output",0x825B7BA8);
}
REX_HOOK_RAW(sub_827117E0) {
  const auto actor=ctx.r3.u32, caller=uint32_t(ctx.lr);
  kk::coop::raptor_route::WriteInput(ctx,base,"position setter input");
  __imp__sub_827117E0(ctx,base);
  kk::coop::raptor_route::WriteOutput(base,actor,caller,"position setter output");
}
REX_HOOK_RAW(sub_82711718) {
  const auto actor=ctx.r3.u32, caller=uint32_t(ctx.lr);
  kk::coop::raptor_route::WriteInput(ctx,base,"position setter input");
  __imp__sub_82711718(ctx,base);
  kk::coop::raptor_route::WriteOutput(base,actor,caller,"position setter output");
}
REX_HOOK_RAW(sub_82710E20) {
  const auto actor=ctx.r3.u32, caller=uint32_t(ctx.lr);
  kk::coop::raptor_route::WriteInput(ctx,base,"orientation setter input",true);
  __imp__sub_82710E20(ctx,base);
  kk::coop::raptor_route::WriteOutput(base,actor,caller,"orientation setter output");
}
REX_HOOK_RAW(sub_8285AB30) { kk::coop::raptor_route::Matrix(ctx,base,false,__imp__sub_8285AB30); }
REX_HOOK_RAW(sub_8285ADE8) { kk::coop::raptor_route::Matrix(ctx,base,true,__imp__sub_8285ADE8); }
REX_HOOK_RAW(sub_82491B50) {
  if (!REXCVAR_GET(kk_coop_enabled) || !kk::coop::raptor_route::Raptor(base)) {__imp__sub_82491B50(ctx,base);return;}
  kk::coop::raptor_route::Scope scope(base);
  kk::coop::raptor_route::Inputs(base,"motion input",uint32_t(ctx.lr));
#if defined(KK_DEV_TOOLS)
  const auto v=kk::coop::raptor_route::current.vars;
  if (v && kk::hfr::LoadF(base,v+8580)>0 && kk::hfr::LoadF(base,v+8604)==0 &&
      kk::coop::raptor_route::Finite(base,v+2584)) {
    const auto own=kk::coop::raptor_route::ReadPoint(base,kk::coop::raptor_route::current.matrix+48);
    const auto step=kk::coop::raptor_route::ReadPoint(base,v+2584);
    if (std::hypot(step[0]-own[0],step[1]-own[1])>2 && kk::coop::raptor_route::Force(base,"path")) {
      // Reuse an existing native destination twice. No actor is moved and no
      // new destination is supplied. Exercise the native zero segment case.
      kk::hfr::Store32(base,v+2644,2);
      for (unsigned row=0;row<2;++row) for(unsigned axis=0;axis<3;++axis)
        kk::hfr::StoreF(base,v+2652+12*row+4*axis,step[axis]);
      kk::hfr::Store32(base,v+6732,kk::hfr::Load32(base,v+6732)|0x1000);
    }
  }
#endif
  __imp__sub_82491B50(ctx,base);
  kk::coop::raptor_route::Inputs(base,"motion output",0x82491B50);
  if (kk::coop::raptor_route::PendingOrientation()) kk::coop::raptor_route::Recover(ctx,base,"orientation input");
  if (kk::coop::raptor_route::PendingCurve() && kk::coop::raptor_route::Recover(ctx,base,"invalid movement curve")) ctx.r3.u64=0;
}
REX_HOOK_RAW(sub_8245B0D0) {
  const auto out=ctx.r3.u32, caller=uint32_t(ctx.lr);
  __imp__sub_8245B0D0(ctx,base);
  kk::coop::raptor_route::Point(base,out,"cell curve output",caller);
}
REX_HOOK_RAW(sub_8261D338) { kk::coop::raptor_route::Bezier(ctx,base); }
REX_HOOK_RAW(sub_82409B80) {
  kk::coop::raptor_route::WriteInput(ctx,base,"velocity setter input");
  __imp__sub_82409B80(ctx,base);
}

REX_HOOK_RAW(sub_825924A8) {
  kk::coop::raptor_route::Scope scope(base);
  kk::coop::raptor_route::Inputs(base,"destination input",uint32_t(ctx.lr));
  __imp__sub_825924A8(ctx,base);
  kk::coop::raptor_route::Inputs(base,"destination output",0x825924A8);
  if (kk::coop::raptor_route::PendingGrid())
    kk::coop::raptor_route::Recover(ctx,base,"destination grid refusal");
}
REX_HOOK_RAW(sub_825757B0) {
  kk::coop::raptor_route::Scope scope(base);
  kk::coop::raptor_route::Inputs(base,"movement input",uint32_t(ctx.lr));
  __imp__sub_825757B0(ctx,base);
  kk::coop::raptor_route::Inputs(base,"movement output",0x825757B0);
}
REX_HOOK_RAW(sub_82572298) {
  kk::coop::raptor_route::Scope scope(base);
#if defined(KK_DEV_TOOLS)
  if (kk::coop::raptor_route::Force(base,"goal")) kk::hfr::Store32(base,kk::coop::raptor_route::current.vars+2572,0x7FC00000);
#endif
  kk::coop::raptor_route::Inputs(base,"compute input",uint32_t(ctx.lr));
  if (kk::coop::raptor_route::BadRoute(base) || kk::coop::raptor_route::PendingOrientation()) {
    kk::coop::raptor_route::Recover(ctx,base,"invalid compute input");
    ctx.r3.u64=0;
    return;
  }
  __imp__sub_82572298(ctx,base);
  kk::coop::raptor_route::Inputs(base,"compute output",0x82572298);
  if (kk::coop::raptor_route::PendingGrid() || kk::coop::raptor_route::BadRoute(base))
    kk::coop::raptor_route::Recover(ctx,base,"compute grid refusal");
}
REX_HOOK_RAW(sub_824759C0) {
  kk::coop::raptor_route::Inputs(base,"collision input",uint32_t(ctx.lr));
  __imp__sub_824759C0(ctx,base);
  kk::coop::raptor_route::Inputs(base,"collision output",0x824759C0);
}
REX_HOOK_RAW(sub_82465920) {
  kk::coop::raptor_route::Inputs(base,"detour input",uint32_t(ctx.lr));
  __imp__sub_82465920(ctx,base);
  kk::coop::raptor_route::Inputs(base,"detour output",0x82465920);
}
REX_HOOK_RAW(sub_826E9220) {
  // Packed input Vec3 values occupy GPR slots. The output pointer is the
  // low word of argument slot 11, at caller SP + 92 (callee SP + 1180).
  const auto out=kk::coop::raptor_route::current.vars ? kk::hfr::Load32(base,ctx.r1.u32+92):0;
  const auto caller=uint32_t(ctx.lr);
  __imp__sub_826E9220(ctx,base);
  kk::coop::raptor_route::Point(base,out,"segment output",caller);
}
REX_HOOK_RAW(sub_826D5CD8) {
  const auto out=ctx.r3.u32, caller=uint32_t(ctx.lr);
  __imp__sub_826D5CD8(ctx,base);
  kk::coop::raptor_route::Point(base,out,"circle steering output",caller);
}

REX_HOOK_RAW(sub_82556890) {
  kk::coop::grid_guard::Line line{ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.f1.f64};
#if defined(KK_DEV_TOOLS)
  if (kk::coop::raptor_route::Force(base,"grid")) line.from_z=uint64_t(0x7FC00000)<<32;
#endif
  if (!kk::coop::grid_guard::Refuse(REXCVAR_GET(kk_coop_enabled), line)) {
    __imp__sub_82556890(ctx, base);
    return;
  }

  // Preserve the native helper's initial scratch flag clear. Do not read an
  // actor, AI or world pointer to reject an invalid query.
  kk::hfr::Store32(base, 0x82CCBFC0, 0);
  if (kk::coop::raptor_route::current.vars) {
    ++kk::coop::raptor_route::grid_refusals;
    kk::coop::raptor_route::refused_grid=kk::coop::raptor_route::current;
  }
  static kk::coop::grid_guard::Reports reports;
  const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if (reports.Note(now)) {
    const auto p = line.Points();
    // These two globals hold identity only. No diagnostic follows the pointers.
    REXLOG_WARN("KK co op grid query: invalid line refused actor={:08X} ai={:08X} caller={:08X} "
        "from=({},{},{}) to=({},{},{}) packed={:016X}/{:016X}/{:016X}/{:016X} "
        "shorten={} mask={:08X} width={} refused={}",
        kk::hfr::Load32(base, 0x82CCBF6C), kk::hfr::Load32(base, 0x82CCBF64), uint32_t(ctx.lr),
        p[0], p[1], p[2], p[3], p[4], p[5], line.from_xy, line.from_z, line.to_xy, line.to_z,
        line.shorten, ctx.r9.u32, ctx.r10.s32, reports.refused);
  }
  ctx.r3.u64 = 0;
}
