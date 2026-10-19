#include "coop_observer.h"
#include "coop_camera.h"
#include "coop_runtime.h"
#include "coop_pose.h"
#include "hfr.h"
#include "play_internal.h"
#include "photo_mode.h"
#include "freecam.h"
#include "settings.h"
#include <rex/hook.h>
#include <rex/logging.h>
#include <atomic>
#include <chrono>
#include <cstring>

REX_EXTERN(__imp__sub_827117E0);
REX_EXTERN(__imp__sub_82401AA8);
namespace kk::coop::observer {
namespace {
double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::atomic<bool> temporary{false}, observing{false}, screen{true}, failed{false};
std::atomic<const char*> status{"Observer: waiting for host camera."};
uint32_t generation=0, normal_matrix=0, normal_actor=0, parked_actor=0, parked_matrix=0;
uint32_t local_checkpoint=0;
double began=0,last_camera=0,normal_time=0,mismatch_since=0,checkpoint_mismatch=0,skip_until=0;
double camera_timeout_since=0,host_capture_time=0;
const char* host_capture_reason="no scene view";
std::optional<CameraPose> host_previous,remote;
uint32_t cut=1,skip=0,consumed_skip=0;
bool start_down=false,local_scripted=false,parked=false;
TogetherCutscene together_scene;
float origin[3]{};

bool World(const uint8_t* b,const FlowView& v) {
  const auto g=hfr::Load32(b,0x82CC97C0);
  return PoseAddress(g,28) && hfr::Load32(b,g+24)==v.world.level && !hfr::Load32(b,g+4);
}
bool SceneCall(uint32_t caller) { return caller==0x82873750 || caller==0x8287390C || caller==0x8287397C; }
bool NormalView(const uint8_t* b,uint32_t view) {
  if (!normal_time || Now()-normal_time>.15 || !PoseAddress(normal_actor,28) ||
      hfr::Load32(b,normal_actor+12)!=normal_matrix || !PoseAddress(normal_matrix,64)) return false;
  for (unsigned row=0;row<4;++row) for (unsigned c=0;c<3;++c) {
    const float a=hfr::LoadF(b,view+row*16+c*4),n=hfr::LoadF(b,normal_matrix+row*16+c*4);
    if (!std::isfinite(a) || !std::isfinite(n) || std::abs(a-n)>.02f) return false;
  }
  return true;
}
bool Movie(const uint8_t* b) {
  const auto p=hfr::Load32(b,0x82CCAE80);
  return PoseAddress(p,0x134) && (hfr::Load32(b,p+0x10c) || hfr::Load32(b,p+0x110));
}
void Reset() {
  normal_matrix=normal_actor=parked_actor=parked_matrix=0; normal_time=0;
  parked=false; host_previous.reset(); remote.reset(); cut=1; skip=consumed_skip=0;
  start_down=false; local_scripted=false; mismatch_since=skip_until=0; failed=false; observing=false; screen=true;
  began=Now(); last_camera=0; checkpoint_mismatch=0; status="Observer: waiting for host camera.";
  camera_timeout_since=host_capture_time=0; host_capture_reason="no scene view";
  together_scene={};
}
}
void SetPartnerSpectating(bool enabled) {
  // Only the request is atomic; the game-thread frame performs state transitions.
  temporary=enabled;
}
void LevelLoaded() { temporary=false; generation=0; Reset(); PublishHostCamera({}); }
void Checkpoint(uint32_t level,uint32_t checkpoint) {
  const auto v=SessionFlowSnapshot();
  if (!v.host && v.policy==ChapterPolicy::Observer && v.world.level==level) local_checkpoint=checkpoint;
}
bool HoldWorld() {
  // The load-boundary wait polls here even when no native GameFrame can run.
  // Only packet expiry is recoverable; script/movie/parking failures stay held.
  if (observing && failed && camera_timeout_since && CameraConnectionReady()) {
    const auto v=SessionFlowSnapshot();
    const auto p=HostCameraSnapshot(Now());
    const auto info=HostCameraDiagnostics();
    if (v.active && !v.host && v.phase==FlowPhase::Playing && v.world.generation==generation && p &&
        p->generation==generation && p->level==v.world.level && info.accepted>camera_timeout_since) {
      remote=p; last_camera=info.accepted; camera_timeout_since=0; mismatch_since=0;
      failed=false; screen=false; status=p->paused ? "Watching host. Host is paused." : "Watching host.";
      REXLOG_INFO("KK co-op observer: host camera recovered, generation {} sequence {}",generation,info.accepted_sequence);
    }
  }
  return observing && failed;
}
bool StatusScreen() { return screen.load(); }
bool Active() { return observing.load(); }
const char* Status() { return status.load(); }
bool SuppressLook() { return REXCVAR_GET(kk_coop_enabled) && observing.load(); }
bool HideWeapon() { return SuppressLook(); }
void Fallback(const char* why) {
  const bool was_timeout=camera_timeout_since!=0;
  camera_timeout_since=0;
  if (!observing || (failed.exchange(true) && !was_timeout)) return;
  screen=true; status="Observer view unavailable. Following host progress.";
  const auto info=HostCameraDiagnostics(); const double now=Now();
  REXLOG_WARN("KK co-op observer: status fallback, generation {}: {} packet-age={:.3f}s accepted-age={:.3f}s sequence={}/{} accepted={} rejected={} last-reject={} reject-age={:.3f}s",
      generation,why,info.received>=0 ? now-info.received : -1,info.accepted>=0 ? now-info.accepted : -1,
      info.sequence,info.accepted_sequence,info.accepted_count,info.rejected_count,info.rejection,
      info.rejected_at>=0 ? now-info.rejected_at : -1);
}
void GameFrame(uint8_t* b) {
  const auto v=SessionFlowSnapshot();
  if (!REXCVAR_GET(kk_coop_enabled) || !v.active || v.phase!=FlowPhase::Playing || !play::SessionActive()) {
    observing=false; temporary=false; together_scene={}; PublishHostCamera({}); return;
  }
  // The flow generation resets local actor identities even on a same-level checkpoint reload.
  if (generation!=v.world.generation) {
    const bool down=temporary; Reset(); temporary=down; generation=v.world.generation; local_checkpoint=v.world.start;
  }
  if (v.host || !CameraConnectionReady()) temporary=false;
  if (v.host && host_previous && (play::PauseMenuOpen() || play::GamePausePageUp())) {
    auto p=*host_previous; p.time=Now(); p.paused=true; PublishHostCamera(p);
  }
  const bool was_cinematic=together_scene.Active();
  const bool was_skip=together_scene.Skip(Now());
  together_scene.Update(!v.host && v.policy==ChapterPolicy::Together && CameraConnectionReady() ?
      HostCameraSnapshot(Now()) : std::optional<CameraPose>{},v.world.generation,v.world.level,Now());
  const bool cinematic=together_scene.Active();
  if (was_cinematic!=cinematic)
    REXLOG_INFO("KK co-op story: host cinematic view {} generation {} level {}; native guest scripts continue",
        cinematic ? "started" : "ended",generation,v.world.level);
  if (!was_skip && together_scene.Skip(Now()))
    REXLOG_INFO("KK co-op story: host cinematic skip received generation {} level {}; forwarding to local cinematic input",
        generation,v.world.level);
  const bool wanted=!v.host && (v.policy==ChapterPolicy::Observer || temporary || cinematic);
  if (wanted && !observing) {
    // Each temporary downed/cinematic view starts a new lease in this chapter.
    failed=false; began=Now(); last_camera=0; mismatch_since=0; remote.reset();
    camera_timeout_since=0;
    REXLOG_INFO("KK co-op observer: camera follow enabled, generation {} level {} temporary={}",
        generation,v.world.level,temporary.load());
  }
  observing=wanted;
  if (!observing) { screen=false; return; }
  if (failed && HoldWorld()) return;
  if (!b || !World(b,v) || play::StartLevelLoading() || play::CoverScreen()) return;
  // Native movie playback is a separate presentation path. Its scene matrix
  // cannot establish matching movie identity or playback time on the guest.
  if (!temporary && !cinematic && Movie(b)) {
    Fallback("Observer unavailable: movie playback has no validated host start/skip synchronization."); return;
  }
  const bool menu=play::PauseMenuOpen() || play::GamePausePageUp();
  if (menu) mismatch_since=checkpoint_mismatch=0;
  if (!menu && !temporary && !cinematic && v.checkpoint!=local_checkpoint) {
    if (!checkpoint_mismatch) checkpoint_mismatch=Now();
    if (Now()-checkpoint_mismatch>2) {
      Fallback("Observer unavailable: local script checkpoint diverged from host."); return;
    }
  } else checkpoint_mismatch=0;
  remote=HostCameraSnapshot(Now());
  if (remote && remote->generation==generation && remote->level==v.world.level) {
    if (!last_camera) {
      consumed_skip=remote->skip;
      REXLOG_INFO("KK co-op observer: host camera acquired, generation {} level {} cut {}",
          generation,remote->level,remote->cut);
    }
    last_camera=HostCameraDiagnostics().accepted;
    if (!failed) { screen=false; status=remote->paused ? "Watching host. Host is paused." : "Watching host."; }
  } else {
    remote.reset(); screen=true; status="Observer: waiting for host camera.";
    if (Now()-(last_camera>0 ? last_camera : began)>kCameraExpirySeconds) {
      Fallback("Observer unavailable: host camera expired or no scene camera in this chapter.");
      camera_timeout_since=Now();
    }
  }
}
void NormalCamera(const uint8_t* b,uint32_t actor,uint32_t caller) {
  if (!REXCVAR_GET(kk_coop_enabled) || caller!=0x824715FC || !PoseAddress(actor,28)) return;
  const auto matrix=hfr::Load32(b,actor+12);
  if (actor!=hfr::Load32(b,0x82CCBF6C) || !PoseAddress(matrix,64)) return;
  normal_actor=actor; normal_matrix=matrix; normal_time=Now();
}
bool View(uint8_t* b,uint32_t view,uint32_t caller,std::array<uint8_t,68>& saved,float& saved_fov,float lens_factor) {
  if (!REXCVAR_GET(kk_coop_enabled) || !b || !SceneCall(caller) || !PoseAddress(view,192) ||
      !play::SessionActive() || play::StartLevelLoading() || play::CoverScreen()) return false;
  const auto v=SessionFlowSnapshot();
  if (!v.active || v.phase!=FlowPhase::Playing || !World(b,v)) return false;
  if (play::PauseMenuOpen() || play::GamePausePageUp()) return false;
  local_scripted=!NormalView(b,view) || Movie(b);
  if (v.host) {
    // Other scene passes must never invalidate the opaque camera publication.
    if (caller!=0x8287390C) return false;
    if (photo::Active() || freecam::Active()) { host_capture_reason="photo/free camera"; return false; }
    CameraPose p; p.generation=v.world.generation; p.level=v.world.level; p.time=Now();
    p.x=hfr::LoadF(b,view+48); p.y=hfr::LoadF(b,view+52); p.z=hfr::LoadF(b,view+56);
    p.fov=hfr::LoadF(b,view+172);
    if (lens_factor!=1) p.fov=2*std::atan(lens_factor*std::tan(p.fov*.5f));
    p.scripted=local_scripted; p.skip=skip;
    CameraAxes axes{};
    for (unsigned i=0;i<3;++i) for (unsigned j=0;j<3;++j) axes[i][j]=hfr::LoadF(b,view+16*i+4*j);
    if (!CameraRotation(p,axes)) { host_capture_reason="invalid scene rotation"; return false; }
    // Capture just the opaque scene setup, before free/photo camera and aspect widening.
    if (host_previous && (p.scripted!=host_previous->scripted ||
        std::hypot(p.x-host_previous->x,p.y-host_previous->y,p.z-host_previous->z)>10 ||
        std::abs(CameraDot(p,*host_previous))<.5f)) ++cut;
    p.cut=cut;
    if (ValidCameraPose(p)) {
      if (host_capture_time && p.time-host_capture_time>.5)
        REXLOG_INFO("KK co-op observer: host capture resumed after {:.3f}s, previous={} caller={:08X}",
            p.time-host_capture_time,host_capture_reason,caller);
      host_capture_time=p.time; host_capture_reason="scene capture/frame gap";
      host_previous=p; PublishHostCamera(p);
    } else host_capture_reason="invalid scene pose/lens";
    return false;
  }
  if (!observing || failed || !remote || remote->generation!=v.world.generation || remote->level!=v.world.level ||
      play::PauseMenuOpen() || play::GamePausePageUp()) return false;
  // Local script cameras must enter/leave naturally from the same load. A stream
  // cannot repair a divergent script world. Do not invoke an arbitrary script from wire.
  // A frozen stream cannot track a still-running local cutscene. Resume the
  // divergence grace period when fresh packets return, rather than latching a
  // script failure solely because the host's render thread hitched.
  const auto info=HostCameraDiagnostics();
  const bool fresh=info.accepted>=0 && Now()-info.accepted<=.5;
  if (!fresh) mismatch_since=0;
  if (!temporary && !together_scene.Active() && fresh && !remote->paused && caller==0x8287390C) {
    bool diverged=local_scripted!=remote->scripted;
    if (local_scripted && remote->scripted) {
      CameraPose local;
      CameraAxes axes{};
      for (unsigned i=0;i<3;++i) for (unsigned j=0;j<3;++j) axes[i][j]=hfr::LoadF(b,view+16*i+4*j);
      const float distance=std::hypot(hfr::LoadF(b,view+48)-remote->x,
          hfr::LoadF(b,view+52)-remote->y,hfr::LoadF(b,view+56)-remote->z);
      // Allow the interpolation delay, never substitute a different local scene
      // merely because both cameras are scripted. Fast or incompatible scenes fail closed.
      diverged=!std::isfinite(distance) || distance>5 || !CameraRotation(local,axes) ||
          std::abs(CameraDot(local,*remote))<.9f;
    }
    if (diverged) {
      if (!mismatch_since) mismatch_since=Now();
      if (Now()-mismatch_since>1.0) { Fallback("Observer unavailable: local cutscene start/end or scene diverged from host."); return false; }
    } else mismatch_since=0;
  }
  std::memcpy(saved.data(),b+view,saved.size()); saved_fov=hfr::LoadF(b,view+172);
  const auto axes=CameraBasis(*remote);
  for (unsigned i=0;i<3;++i) for (unsigned j=0;j<3;++j) hfr::StoreF(b,view+16*i+4*j,axes[i][j]);
  hfr::StoreF(b,view+48,remote->x); hfr::StoreF(b,view+52,remote->y); hfr::StoreF(b,view+56,remote->z);
  hfr::StoreF(b,view+172,remote->fov); return true;
}
void FilterPad(uint32_t user,uint8_t* state) {
  if (!REXCVAR_GET(kk_coop_enabled)) return;
  const auto v=SessionFlowSnapshot();
  if (!v.active || v.phase!=FlowPhase::Playing) return;
  const uint16_t buttons=uint16_t(state[4])<<8|state[5];
  const bool down=(buttons&0x10)!=0;
  if (v.host) {
    if (user) return;
    if (down && !start_down && local_scripted && !play::PauseMenuOpen()) {
      ++skip;
      REXLOG_INFO("KK co-op story: host cinematic skip queued generation {} level {} skip={}",generation,v.world.level,skip);
    }
    start_down=down; return;
  }
  if (play::PauseMenuOpen() || play::GamePausePageUp()) return;
  const bool together_skip=!user && v.policy==ChapterPolicy::Together && together_scene.Skip(Now()) && local_scripted;
  if (!observing) { if (together_skip) state[5]|=0x10; return; }
  // Pause remains available in play. During a cutscene only the host can skip.
  const uint16_t allowed=local_scripted ? 0 : buttons&0x30;
  std::memset(state+4,0,12); state[4]=uint8_t(allowed>>8); state[5]=uint8_t(allowed);
  if (together_skip) state[5]|=0x10;
  if (remote && !local_scripted) { consumed_skip=remote->skip; skip_until=0; }
  if (!user && remote && remote->skip>consumed_skip && local_scripted) {
    consumed_skip=remote->skip; skip_until=Now()+.12;
  }
  if (!user && local_scripted && Now()<skip_until) state[5]|=0x10;
}
void LateWorld(PPCContext& ctx,uint8_t* b) {
  if (!observing || failed || !b || play::PauseMenuOpen() || play::StartLevelLoading() || play::CoverScreen()) return;
  // Downed and cinematic viewers keep their native actor/physics intact.
  if (temporary || together_scene.Active()) return;
  // Let the native chapter-start drop settle before fixing an on-foot origin.
  // Gameplay input is already suppressed throughout this short settling period.
  if (Now()-began<1) return;
  const auto v=SessionFlowSnapshot(); if (!World(b,v)) return;
  const auto pose=ReadJackPose([&](uint32_t a){return hfr::Load32(b,a);},[&](uint32_t a){return hfr::Load16(b,a);},
      [&](uint32_t a){return hfr::LoadF(b,a);},Now());
  // Kong/credits have no validated Jack parking ABI. Scripted scenes can run
  // without parking a Jack; a freely controlled unsupported hero must fall back.
  if (!pose.pose) {
    if (last_camera && !local_scripted) Fallback("Observer unavailable: no validated Jack parking path for this hero.");
    return;
  }
  const auto actor=hfr::Load32(b,0x82CCBF7C),matrix=hfr::Load32(b,actor+12);
  const auto ext=hfr::Load32(b,actor+24);
  const auto collision=PoseAddress(ext,4) ? hfr::Load32(b,ext) : 0;
  const auto controller=PoseAddress(collision,8) ? hfr::Load32(b,collision+4) : 0;
  const auto collision_matrix=PoseAddress(controller,60) ? hfr::Load32(b,controller+56) : 0;
  if (!PoseAddress(matrix,80) || !PoseAddress(collision_matrix,60)) {
    Fallback("Observer unavailable: native Jack collision/transform span is invalid."); return;
  }
  if (!parked) {
    parked=true; parked_actor=actor; parked_matrix=matrix;
    origin[0]=pose.pose->x; origin[1]=pose.pose->y; origin[2]=pose.pose->z;
  }
  if (actor!=parked_actor || matrix!=parked_matrix ||
      std::hypot(pose.pose->x-origin[0],pose.pose->y-origin[1],pose.pose->z-origin[2])>2.f) {
    Fallback("Observer unavailable: local script moved/replaced parked Jack."); return;
  }
  // Native position and collision setters, with an isolated guest scratch frame.
  if ((ctx.r1.u32&15) || ctx.r1.u32<0x40014000 || ctx.r1.u32>=0xA0000000) {
    Fallback("Observer unavailable: native parking stack is invalid."); return;
  }
  const auto saved=ctx; const uint32_t at=ctx.r1.u32-16384;
  std::array<uint8_t,16384> bytes; std::memcpy(bytes.data(),b+at,bytes.size());
  ctx.r1.u64=at+12288; std::memset(b+at+12288,0,4096); hfr::Store32(b,at+12288,saved.r1.u32);
  const auto xyz=at+12544;
  for (unsigned i=0;i<3;++i) hfr::StoreF(b,xyz+i*4,origin[i]);
  ctx.r3.u64=actor; ctx.r4.u64=xyz; __imp__sub_827117E0(ctx,b);
  ctx.r3.u64=actor; ctx.r4.u64=xyz; __imp__sub_82401AA8(ctx,b);
  std::memcpy(b+at,bytes.data(),bytes.size()); ctx=saved;
}
} // namespace kk::coop::observer
