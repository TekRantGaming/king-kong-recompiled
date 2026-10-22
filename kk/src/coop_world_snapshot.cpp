#if defined(KK_DEV_TOOLS)
// KK_DEV_WORLD_DUMP=25,2,16: delay after Playing, interval, bounded count.
// KK_DEV_WORLD_DIR=<private directory>; each peer needs its own directory.
// KK_DEV_WORLD_ROUTE=12: stop existing wander input after 12 Playing seconds.
// No gameplay/AI freeze. Invalid snapshots are retained with exact reasons.
#include "coop_world_snapshot.h"
#include "coop_world_build.h"
#include "coop_runtime.h"
#include "coop_pose.h"
#include "coop_enemies_game.h"
#include "coop_observer.h"
#include "play_internal.h"
#include "hfr.h"
#include <rex/logging.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

namespace kk::coop::world {
namespace {
double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
struct Options {
  bool enabled=false;
  double delay=25,interval=2,route=12;
  unsigned count=16;
  std::filesystem::path dir;
  Options() {
    const auto* value=std::getenv("KK_DEV_WORLD_DUMP");
    const auto* path=std::getenv("KK_DEV_WORLD_DIR");
    if (!value || !path) return;
    char tail=0;
    if (std::sscanf(value,"%lf,%lf,%u%c",&delay,&interval,&count,&tail)!=3 ||
        !std::isfinite(delay) || !std::isfinite(interval) || delay<1 || interval<.5 || count<1 || count>120) {
      REXLOG_WARN("KK dev world: invalid schedule"); return;
    }
    if (const auto* r=std::getenv("KK_DEV_WORLD_ROUTE")) {
      char* end=nullptr; route=std::strtod(r,&end);
      if (!end || *end || !std::isfinite(route) || route<0 || route>delay) return;
    }
    dir=std::filesystem::absolute(path).lexically_normal();
    auto normalized=dir.generic_string();
    std::transform(normalized.begin(),normalized.end(),normalized.begin(),[](unsigned char c){ return char(std::tolower(c)); });
    // Diagnostics must never write to the owner's renderer tree or Documents.
    if (!normalized.starts_with("f:/kk-native-coop/")) { REXLOG_WARN("KK dev world: refused output path"); return; }
    enabled=true;
  }
};
Options& Opt() { static Options opt; return opt; }
uint64_t epoch=0;
uint32_t generation=0,level=0;
unsigned sample=0;
double playing_at=0,next=0;
Json Position(const PlayerPose& p) { return Json::Array({p.x,p.y,p.z}); }
Json Checkpoints(const uint8_t* b,Capture& capture) {
  using namespace hfr;
  const auto w=Load32(b,0x828E8388);
  if (!PoseAddress(w,44)) { capture.Error("checkpoint-world-unreadable"); return {}; }
  const auto begin=Load32(b,w+40),end=Load32(b,w+28);
  if (!PoseAddress(begin) || !PoseAddress(end) || end<begin || (end-begin)%8 || end-begin>8*20000) {
    capture.Error("checkpoint-registry-unreadable"); return {};
  }
  std::set<uint32_t> ids;
  for (auto p=begin;p<end;p+=8) {
    const auto actor=Load32(b,p);
    if (!PoseAddress(actor,28)) continue;
    const auto ext=Load32(b,actor+24); if (!PoseAddress(ext,8)) continue;
    const auto ai=Load32(b,ext+4); if (!PoseAddress(ai,88)) continue;
    const auto first=Load16(b,ai+80),last=Load16(b,ai+82);
    if (first>last || last>4096 || last-first>=64 || !PoseAddress(ai,(last+12)*4)) continue;
    for (unsigned t=first;t<=last;++t) {
      const auto block=Load32(b,ai+(t+6)*4);
      if (!PoseAddress(block,24) || Load32(b,block+20)!=0x826437D0) continue;
      const auto vars=Load32(b,ai+64);
      if (!PoseAddress(vars,8)) { capture.Error("checkpoint-vars-unreadable"); continue; }
      const auto id=Load32(b,vars+4);
      if (id>65535) capture.Error("checkpoint-id-out-of-range"); else ids.insert(id);
    }
  }
  std::vector<Json> rows; for (const auto id:ids) rows.emplace_back(id);
  return Json::Array(rows);
}
Json Snapshot(uint8_t* b,double now,const FlowView& v,const EnemyLink& link) {
  Capture capture;
  const auto pose=ReadJackPose([&](uint32_t a){return hfr::Load32(b,a);},
      [&](uint32_t a){return hfr::Load16(b,a);},[&](uint32_t a){return hfr::LoadF(b,a);},now);
  if (!pose.pose) capture.Error(std::string("native-player:")+pose.failure);
  if (!link.SessionConnected() || !link.connected) capture.Error("peer-not-fresh-connected");
  if (!link.partner || now-link.partner->time>1 || link.partner->level!=v.world.level) capture.Error("partner-pose-unavailable-or-stale");
  if (play::StartLevelLoading() || play::CoverScreen() || SessionPrivateReload() || SessionCheckpointCatchup()) capture.Error("native-load-not-ready");
  if (play::PauseMenuOpen() || play::GamePausePageUp() || observer::InCutscene(b)) capture.Error("paused-or-cutscene");
  if (LocalPlayerCondition(b)==PlayerCondition::Dead) capture.Error("native-player-dead");
  const auto ew=EnemyCurrentWorld();
  if (!ValidEnemyWorld(ew) || ew.level!=v.world.level || ew.start!=v.world.start) capture.Error("native-enemy-world-mismatch");
  const auto g=hfr::Load32(b,0x82CC97C0);
  if (!PoseAddress(g,28) || hfr::Load32(b,g+24)!=v.world.level) capture.Error("native-level-mismatch");
  auto enemies=EnemySnapshot(b,capture),story=StorySnapshot(b,capture),body=BodySnapshot(b,capture);
  const auto checkpoints=Checkpoints(b,capture);
  const auto env=[](const char* name){ const auto* value=std::getenv(name); return value ? value:""; };
  const auto utc=std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
  const bool together=v.policy==ChapterPolicy::Together;
  if (!together) capture.Error("policy-allows-independent-worlds");
  std::vector<Json> errors; for (const auto& error:capture.errors) errors.emplace_back(error);
  return Json::Object({{"schema",1},{"protocol",kProtocolVersion},{"sample",sample},
      {"captured_utc",utc},{"playing_seconds",now-playing_at},{"role",v.host ? "host":"guest"},
      {"identity",Json::Object({{"session",Hex(link.epoch)},{"generation",v.world.generation},
          {"level",v.world.level},{"start",v.world.start},{"enemy_visit",ew.serial},
          {"checkpoint",SessionNativeCheckpoint()},{"shared_checkpoint",v.checkpoint},
          {"completed",Hex(v.completed)},{"policy",together ? "together":v.policy==ChapterPolicy::SoloRegroup ? "solo-regroup":"observer"}})},
      {"evidence",Json::Object({{"build_commit",KK_WORLD_SOURCE_COMMIT},{"source_commit",env("KK_DEV_WORLD_COMMIT")},
          {"source_dirty",env("KK_DEV_WORLD_DIRTY")},{"exe_sha256",env("KK_DEV_WORLD_EXE_SHA256")},
          {"scenario",env("KK_DEV_WORLD_SCENARIO")},{"fault_profile",env("KK_DEV_UDP_SIM")}})},
      {"coverage",Json::Object({{"objects","unsupported:C71-not-integrated"},{"broad_story_flags","unsupported"},
          {"streaming_collision_visibility","unsupported"},{"enemies","native-bound-and-unbound-census"},
          {"scripts","native-cursor-and-track;run-metadata-separate"}})},
      {"valid",capture.errors.empty()},{"errors",Json::Array(errors)},
      {"shared",Json::Object({{"enemies",enemies},{"story",story}})},
      {"native_checkpoint_ids",checkpoints},
      {"local",Json::Object({{"player",pose.pose ? Json::Object({{"id",v.host ? "host":"guest"},
          {"position",Position(*pose.pose)},{"weapon",pose.pose->weapon},{"fire",ConfirmedShots()},
          {"condition",unsigned(LocalPlayerCondition(b).value_or(PlayerCondition::Dead))}}):Json()},
          {"partner_body",body},{"pickups_policy","personal"}})}});
}
}
bool RouteStopped() { return Opt().enabled && playing_at && Now()-playing_at>=Opt().route; }
void Frame(uint8_t* b) {
  auto& opt=Opt(); if (!opt.enabled || !b) return;
  const auto v=SessionFlowSnapshot(); const auto link=EnemyLinkSnapshot(); const double now=Now();
  if (epoch!=link.epoch || generation!=v.world.generation || level!=v.world.level) {
    epoch=link.epoch; generation=v.world.generation; level=v.world.level; playing_at=next=0; sample=0;
  }
  if (!v.active || v.phase!=FlowPhase::Playing || !epoch || !generation) return;
  if (!playing_at) { playing_at=now; next=now+opt.delay; }
  if (sample>=opt.count || now<next) return;
  ++sample; next=now+opt.interval;
  try {
    std::filesystem::create_directories(opt.dir);
    const auto path=opt.dir/("world-g"+std::to_string(generation)+"-"+std::to_string(sample)+".json");
    const auto json=Snapshot(b,now,v,link);
    std::ofstream out(path,std::ios::binary); out<<json.text<<'\n'; out.close();
    if (!out) REXLOG_WARN("KK dev world: write failed sample={}",sample);
    else REXLOG_INFO("KK dev world: dumped sample={} generation={} level={} path={}",sample,generation,level,path.string());
  } catch (const std::exception& e) { REXLOG_WARN("KK dev world: dump failed {}",e.what()); }
}
}
#endif
