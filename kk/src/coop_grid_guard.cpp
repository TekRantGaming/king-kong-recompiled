// C133: the saved 28f hang is a raptor grid walk with NaN coordinates, not
// an established driver stall. Its two loop limits became INT_MAX. Refuse
// an invalid line before conversion; the native return value 0 means blocked.
#include "coop_grid_guard.h"
#include "hfr.h"
#include "coop_pose.h"
#include "coop_stimulus_debug.h"
#include <chrono>
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

namespace kk::coop::raptor_route {
using namespace kk::hfr;
struct Owner { uint32_t actor=0, ai=0, vars=0, matrix=0; };
thread_local Owner current;
struct Scope {
  Owner saved=current;
  explicit Scope(uint8_t* b) {
    current={};
    if (!REXCVAR_GET(kk_coop_enabled)) return;
    const auto actor=Load32(b,0x82CCBF6C), ai=Load32(b,0x82CCBF64);
    if (!PoseAddress(actor,28) || !PoseAddress(ai,68)) return;
    const auto vars=Load32(b,ai+64), matrix=Load32(b,actor+12);
    if (PoseAddress(vars,14240) && PoseAddress(matrix,64)) current={actor,ai,vars,matrix};
  }
  ~Scope() { current=saved; }
};
bool Finite(const uint8_t* b,uint32_t p) {
  return std::isfinite(LoadF(b,p)) && std::isfinite(LoadF(b,p+4)) && std::isfinite(LoadF(b,p+8));
}
void Point(const uint8_t* b,uint32_t p,const char* source,uint32_t caller) {
  if (!current.vars || !PoseAddress(p,12) || Finite(b,p)) return;
  static grid_guard::Reports reports;
  const double now=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if (!reports.Note(now)) return;
  const auto v=current.vars;
  const auto event=stimulus_debug::ReadHandle([&](uint32_t a){return Load32(b,a);},v+5216);
  REXLOG_WARN("KK co op raptor invalid: source={} actor={:08X} ai={:08X} vars={:08X} caller={:08X} "
      "point={:08X} value=({},{},{}) bits={:08X}/{:08X}/{:08X} action={} flags={:08X} "
      "event={:08X} target={:08X} follow={:08X} node={:08X} count={} refused={}",
      source,current.actor,current.ai,v,caller,p,LoadF(b,p),LoadF(b,p+4),LoadF(b,p+8),
      Load32(b,p),Load32(b,p+4),Load32(b,p+8),Load32(b,v+6756),Load32(b,v+6732),
      event.id,event.target,Load32(b,v+7148),Load32(b,v+7152),Load32(b,v+2644),reports.refused);
}
void Inputs(const uint8_t* b,const char* source,uint32_t caller) {
  if (!current.vars) return;
  const auto v=current.vars;
  Point(b,current.matrix+48,"actor position",caller);
  Point(b,v+2572,source,caller);
  Point(b,v+2584,"retained step",caller);
  const auto event=stimulus_debug::ReadHandle([&](uint32_t a){return Load32(b,a);},v+5216);
  if (event.valid) Point(b,event.address+32,"threat event",caller);
  for (auto offset : {7148u,7152u}) {
    const auto object=Load32(b,v+offset);
    if (!PoseAddress(object,16)) continue;
    const auto matrix=Load32(b,object+12);
    if (PoseAddress(matrix,64)) Point(b,matrix+48,offset==7148 ? "follow position":"waypoint position",caller);
  }
  const auto count=Load32(b,v+2644);
  if (count<=100) for (uint32_t i=0;i<count;++i) Point(b,v+2652+12*i,"cached path",caller);
}
} // namespace kk::coop::raptor_route

REX_HOOK_RAW(sub_825924A8) {
  kk::coop::raptor_route::Scope scope(base);
  kk::coop::raptor_route::Inputs(base,"destination input",uint32_t(ctx.lr));
  __imp__sub_825924A8(ctx,base);
  kk::coop::raptor_route::Inputs(base,"destination output",0x825924A8);
}
REX_HOOK_RAW(sub_825757B0) {
  kk::coop::raptor_route::Scope scope(base);
  kk::coop::raptor_route::Inputs(base,"movement input",uint32_t(ctx.lr));
  __imp__sub_825757B0(ctx,base);
  kk::coop::raptor_route::Inputs(base,"movement output",0x825757B0);
}
REX_HOOK_RAW(sub_82572298) {
  kk::coop::raptor_route::Scope scope(base);
  kk::coop::raptor_route::Inputs(base,"compute input",uint32_t(ctx.lr));
  __imp__sub_82572298(ctx,base);
  kk::coop::raptor_route::Inputs(base,"compute output",0x82572298);
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
  const kk::coop::grid_guard::Line line{ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.f1.f64};
  if (!kk::coop::grid_guard::Refuse(REXCVAR_GET(kk_coop_enabled), line)) {
    __imp__sub_82556890(ctx, base);
    return;
  }

  // Preserve the native helper's initial scratch flag clear. Do not read an
  // actor, AI or world pointer to reject an invalid query.
  kk::hfr::Store32(base, 0x82CCBFC0, 0);
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
