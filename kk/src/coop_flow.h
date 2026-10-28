#pragma once

#include "coop_session.h"
#include "coop_revive.h"
#include "coop_handover.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#if defined(KK_DEV_TOOLS)
#include <charconv>
#include <string_view>
#endif

namespace kk::coop {
enum class ChapterPolicy { Together, SoloRegroup, Observer };
enum class DeathRoute { LocalCheckpoint, TogetherRecovery };
inline constexpr DeathRoute DeathRouteFor(ChapterPolicy policy) {
  return policy==ChapterPolicy::Together ? DeathRoute::TogetherRecovery : DeathRoute::LocalCheckpoint;
}
struct ChapterRule { uint32_t level; ChapterPolicy policy; bool uncertain; };
// C29 static-data recommendations: 22 together, 17 solo/regroup, 3 observer.
// Confidence describes source/data evidence only. Every row remains uncertain
// until runtime acceptance; observer streams the host camera with a status fallback.
inline constexpr std::array<ChapterRule, 42> kChapterRules{{
  {100,ChapterPolicy::SoloRegroup,true},   // 0: VERIFIED (data)
  {101,ChapterPolicy::Together,true},      // 1: LIKELY
  {201,ChapterPolicy::Together,true},      // 2: LIKELY
  {203,ChapterPolicy::Together,true},      // 3: LIKELY
  {204,ChapterPolicy::Together,true},      // 4: LIKELY
  {301,ChapterPolicy::Together,true},      // 5: LIKELY
  {302,ChapterPolicy::Together,true},      // 6: LIKELY
  {303,ChapterPolicy::Together,true},      // 7: LIKELY
  {305,ChapterPolicy::Together,true},      // 8: LIKELY
  {306,ChapterPolicy::Together,true},      // 9: LIKELY
  {304,ChapterPolicy::SoloRegroup,true},   // 10: VERIFIED (data)
  {401,ChapterPolicy::Together,true},      // 11: LIKELY
  {402,ChapterPolicy::Together,true},      // 12: LIKELY
  {403,ChapterPolicy::SoloRegroup,true},   // 13: LIKELY
  {404,ChapterPolicy::SoloRegroup,true},   // 14: VERIFIED (data)
  {501,ChapterPolicy::SoloRegroup,true},   // 15: VERIFIED (data)
  {502,ChapterPolicy::SoloRegroup,true},   // 16: VERIFIED (data)
  {504,ChapterPolicy::SoloRegroup,true},   // 17: VERIFIED (data)
  {702,ChapterPolicy::Together,true},      // 18: LIKELY
  {705,ChapterPolicy::SoloRegroup,true},   // 19: VERIFIED (data)
  {901,ChapterPolicy::SoloRegroup,true},   // 20: VERIFIED (data)
  {1002,ChapterPolicy::SoloRegroup,true},  // 21: VERIFIED (data)
  {1101,ChapterPolicy::Together,true},     // 22: LIKELY
  {1200,ChapterPolicy::Together,true},     // 23: LIKELY
  {1201,ChapterPolicy::Together,true},     // 24: LIKELY
  {703,ChapterPolicy::Together,true},      // 25: LIKELY
  {1401,ChapterPolicy::Together,true},     // 26: LIKELY
  {1402,ChapterPolicy::SoloRegroup,true},  // 27: LIKELY (hero disputed)
  {1501,ChapterPolicy::Together,true},     // 28: LIKELY
  {1701,ChapterPolicy::Together,true},     // 29: LIKELY
  {1702,ChapterPolicy::Together,true},     // 30: LIKELY
  {1703,ChapterPolicy::SoloRegroup,true},  // 31: VERIFIED (data)
  {1801,ChapterPolicy::SoloRegroup,true},  // 32: VERIFIED (data)
  {1803,ChapterPolicy::SoloRegroup,true},  // 33: VERIFIED (data)
  {1804,ChapterPolicy::Together,true},     // 34: LIKELY
  {1805,ChapterPolicy::Together,true},     // 35: LIKELY
  {1901,ChapterPolicy::SoloRegroup,true},  // 36: VERIFIED (data)
  {1902,ChapterPolicy::Observer,true},     // 37: GUESS (playability unknown)
  {2001,ChapterPolicy::SoloRegroup,true},  // 38: VERIFIED (data)
  {2003,ChapterPolicy::SoloRegroup,true},  // 39: VERIFIED (data)
  {2004,ChapterPolicy::Observer,true},     // 40: VERIFIED (data)
  {7,ChapterPolicy::Observer,true}         // 41: VERIFIED (data)
}};
#if defined(KK_DEV_TOOLS)
using PolicyOverrides = std::array<std::optional<ChapterPolicy>, kChapterRules.size()>;
// Init-only cvar, installed before either worker starts. Never mutate during play.
inline PolicyOverrides dev_policy_overrides{};
inline std::optional<PolicyOverrides> ParsePolicyOverrides(std::string_view text) {
  PolicyOverrides parsed{};
  if (text.size() > 1024) return {};
  while (!text.empty()) {
    const auto comma = text.find(',');
    const auto item = text.substr(0,comma);
    const auto colon = item.find(':');
    if (colon == std::string_view::npos || colon == 0) return {};
    unsigned entry = 0;
    const auto number = item.substr(0,colon);
    const auto result = std::from_chars(number.data(),number.data()+number.size(),entry);
    if (result.ec != std::errc{} || result.ptr != number.data()+number.size() || entry >= parsed.size()) return {};
    const auto mode = item.substr(colon+1);
    if (mode == "together") parsed[entry] = ChapterPolicy::Together;
    else if (mode == "solo") parsed[entry] = ChapterPolicy::SoloRegroup;
    else if (mode == "observer") parsed[entry] = ChapterPolicy::Observer;
    else return {};
    if (comma == std::string_view::npos) break;
    text.remove_prefix(comma+1);
    if (text.empty()) return {};
  }
  return parsed;  // Empty clears overrides; repeated entries use the last value.
}
#endif
inline int FlowChapter(uint32_t level) {
  for (size_t i=0; i<kChapterRules.size(); ++i) if (kChapterRules[i].level == level) return int(i);
  return -1;
}
inline ChapterPolicy PolicyFor(uint32_t level) {
  const int c = FlowChapter(level);
#if defined(KK_DEV_TOOLS)
  if (c >= 0 && dev_policy_overrides[size_t(c)]) return *dev_policy_overrides[size_t(c)];
#endif
  return c >= 0 ? kChapterRules[size_t(c)].policy : ChapterPolicy::Observer;
}
enum class FlowPhase { Idle, Preparing, Loading, Playing, Waiting, Failed };
struct FlowView {
  bool active = false, start_ready = false, host = false;
  FlowPhase phase = FlowPhase::Idle;
  SessionStart start;
  std::vector<uint8_t> save;
  FlowEvent world;
  uint32_t checkpoint = 0;
  uint64_t completed = 0;
  ChapterPolicy policy = ChapterPolicy::Together;
  const char* status = "";
  revive::PeerLife partner_life;
  bool reconnecting = false, reconnect_expired = false;
  uint32_t rejoin_revision = 0;
  // This world restarts the previous world's level at a checkpoint after a
  // coordinated ReloadRequest (death or scripted fail), not a story transition.
  bool reload = false;
};
// The shared world keeps its original start for actor identity and control
// messages. A returning peer loads the host's current checkpoint instead.
inline uint32_t SessionLoadPoint(const FlowView& v) {
  return !COOP_MUT_IF("rejoin_load_point") && v.start.rejoin &&
      v.start.generation==v.world.generation ? v.start.start : v.world.start;
}
template<class Store>
void RestoreRejoinEquipment(const SessionStart& start,uint32_t g,Store store) {
  if (COOP_MUT_IF("rejoin_equipment")) return;
  store(g+13128,start.weapon);
  for (unsigned i=0;i<4;++i) {
    store(g+13432+i*72,start.stock[i]);
    store(g+14880+i*72,start.stock[i+4]);
  }
}
template<class Load>
bool RejoinEquipmentMatches(const SessionStart& start,uint32_t g,Load load) {
  if (load(g+13128)!=start.weapon) return false;
  for (unsigned i=0;i<4;++i)
    if (load(g+13432+i*72)!=start.stock[i] || load(g+14880+i*72)!=start.stock[i+4]) return false;
  return true;
}
// Continuous evidence only; loading, stale poses or solo policy reset the timer.
class BehindRule {
 public:
  bool Tick(double now, const std::optional<PlayerPose>& local, const std::optional<PlayerPose>& host) {
    if (!local || !host || now-local->time > .5 || !std::isfinite(now)) { since_.reset(); return false; }
    const bool is_far = local->level != host->level || std::hypot(local->x-host->x, local->y-host->y, local->z-host->z) > 100.f;
    if (!is_far) { since_.reset(); return false; }
    if (!since_ || now < *since_) since_ = now;
    if (now-*since_ < 8) return false;
    since_.reset(); return true;
  }
 private:
  std::optional<double> since_;
};

// Host checkpoint notifications update the retry save, not the guest's live
// world. Ordinary catch-up tries each newer host checkpoint at most once.
// Proven world drift may also return to the same/older authoritative checkpoint.
// Track actual native checkpoints/loads separately; a failed attempt never loops.
class CheckpointCatchup {
 public:
  void Reset(uint32_t generation,uint32_t checkpoint) {
    *this={}; generation_=generation; world_=host_=checkpoint;
  }
  void Observe(uint32_t generation,uint32_t checkpoint,double now) {
    if (generation!=generation_) { Reset(generation,checkpoint); return; }
    if (checkpoint!=host_) { host_=checkpoint; offered_until_=now+6; attempted_=false; }
  }
  void NativeCheckpoint(uint32_t checkpoint) { world_=checkpoint; }
  bool Available() const { return host_>world_ && !attempted_; }
  bool Offer(bool script_behind,bool safe,double now) const {
    return !attempted_ && offered_until_>=0 && host_>=world_ && script_behind && safe && std::isfinite(now) &&
        now>=offered_until_-6 && now<=offered_until_;
  }
  bool Begin(bool checkpoint_offer=false,bool desync_recovery=false) {
    if (!Available() && !(!attempted_ && desync_recovery) &&
        !(!attempted_ && host_==world_ && checkpoint_offer && offered_until_>=0)) return false;
    attempted_=true; return true;
  }
  uint32_t Target() const { return host_; }
  uint32_t WorldCheckpoint() const { return world_; }
 private:
  uint32_t generation_=0,world_=0,host_=0;
  double offered_until_=-1;
  bool attempted_=false;
};

// C123: the guest mirrors each host checkpoint into its retry point G+0xC.
// Right after a load, native ES_EnterMap (82643978) reads G+0xC once: only the
// checkpoint object whose start point equals it runs that start point's setup
// (activations, placements). Its ES_CheckPoint pass follows one AI pass later.
// The host's own load can briefly report an older checkpoint (level 201 start 4
// runs 4 -> 3 -> 4 in one frame), and that message may reach the guest before
// its ES_EnterMap pass. Hold the mirror until the guest's own first
// ES_CheckPoint pass of this load, or a bounded number of AI passes.
class LoadMirrorGate {
 public:
  static constexpr unsigned kFallbackPasses=30;
  void Loaded() { settled_=false; passes_=0; }
  void NativeCheckpointPass() { settled_=true; }
  void AiPass() { if (passes_<kFallbackPasses) ++passes_; }
  bool Ready() const { return COOP_MUT_IF("mirror_gate") || settled_ || passes_>=kFallbackPasses; }
 private:
  bool settled_=true;
  unsigned passes_=0;
};

// Deterministic controller, owned by one worker through FlowMailbox. No game API,
// guest memory, filesystem, Steam calls or UI callbacks in this state machine.
class SessionFlow {
 public:
  explicit SessionFlow(bool host = false) { view_.host = host; }
  const FlowView& View() const { return view_; }
  const std::deque<Message>& Outgoing() const { return outgoing_; }
  void Sent() { outgoing_.pop_front(); }
  void Fail(const char* reason) { if (view_.active) { view_.phase = FlowPhase::Failed; view_.status = reason; } }
  void PublishRejoinState(const SessionStart& s) {
    if (view_.host && view_.active && s.level==view_.world.level && ValidSessionStart(s)) checkpoint_state_=s;
  }
  void Connection(SessionState state, uint32_t reconnects, double now) {
    if (!view_.active || view_.phase==FlowPhase::Failed) return;
    if (state==SessionState::Lost || state==SessionState::Connecting) {
      rejoin_requested_=false;
      view_.reconnecting=true;
      if (view_.host && view_.phase==FlowPhase::Loading && local_loaded_) Release();
      view_.status=view_.host ? "Partner disconnected. You can keep playing while they reconnect." :
          "Connection lost. Waiting for the host to reconnect...";
    } else if (state==SessionState::Ended) {
      view_.reconnect_expired=true; view_.reconnecting=!view_.host;
      view_.status="Couldn't reach your partner. Back to menu is available.";
      if (!view_.host) view_.phase=FlowPhase::Failed;
    } else if (state==SessionState::Connected && (view_.reconnecting || reconnects!=reconnects_)) {
      reconnects_=reconnects;
      if (view_.host) {
        Rejoin(now); view_.reconnecting=false;
        view_.status="Partner is catching up. You can keep playing.";
      } else if (!rejoin_requested_) {
        // A one sided interruption need not make the host's session Lost.
        // Ask explicitly rather than waiting for a host transition it never saw.
        Emit(MessageType::RejoinRequest,view_.world); rejoin_requested_=true; rejoin_deadline_=now+120;
      }
    }
  }
  void Rejoin(double now) {
    if (!view_.host || !view_.active) return;
    if (view_.phase!=FlowPhase::Playing || !checkpoint_state_ || checkpoint_state_->generation!=view_.world.generation) {
      rejoin_deferred_=true; return;
    }
    rejoin_deferred_=false;
    ++view_.rejoin_revision;
    auto s=checkpoint_state_.value_or(view_.start);
    s.rejoin=true; s.chapter=uint16_t(FlowChapter(view_.world.level));
    s.level=view_.world.level; s.start=view_.checkpoint;
    s.generation=view_.world.generation; s.world_start=view_.world.start; s.completed=view_.completed;
    s.save_bytes=uint32_t(view_.save.size()); s.save_hash=SessionSaveHash(view_.save);
    outgoing_.clear(); QueueStart(s,view_.save);
    guest_rejoining_=true; rejoin_world_=view_.world; rejoin_deadline_=now+120;
  }
  bool Begin(SessionStart start, std::vector<uint8_t> save, double now) {
    if (!view_.host || view_.active || !ValidSessionStart(start) || start.save_bytes != save.size() ||
        start.save_hash != SessionSaveHash(save) || FlowChapter(start.level) != start.chapter) return false;
    QueueStart(start,save);
    Setup(start, now); view_.save = std::move(save); view_.start_ready = true; return true;
  }
  void Local(Message m, double now) { Handle(m, true, now); }
  void Remote(const Message& m, double now) { Handle(m, false, now); }
  void Tick(double now) {
    if (rejoin_deferred_ && view_.phase==FlowPhase::Playing && checkpoint_state_ &&
        checkpoint_state_->generation==view_.world.generation) Rejoin(now);
    if (!view_.host && view_.reconnecting && rejoin_requested_ && now>=rejoin_deadline_) {
      view_.reconnect_expired=true; view_.phase=FlowPhase::Failed;
      view_.status="Couldn't catch up with the host. Back to menu is available.";
    }
    if (guest_rejoining_ && now>=rejoin_deadline_) {
      guest_rejoining_=false; view_.reconnect_expired=true;
      view_.status="Partner could not catch up. You can keep playing or go back to menu.";
    }
    if (view_.active && (view_.phase==FlowPhase::Preparing || view_.phase==FlowPhase::Loading) && now-deadline_ >= 120) {
      Emit(MessageType::FlowAbort, view_.world); Fail("Co-op timed out waiting for partner. Return to the launcher.");
    }
  }
 private:
  void QueueStart(const SessionStart& start, const std::vector<uint8_t>& save) {
    Message m; m.type=MessageType::SessionStart; m.start=start; outgoing_.push_back(m);
    for (uint32_t offset=0; offset<save.size(); offset+=kSaveChunkBytes) {
      Message chunk; chunk.type=MessageType::SaveChunk; chunk.save_chunk.offset=offset;
      const size_t end=std::min(save.size(),size_t(offset+kSaveChunkBytes));
      chunk.save_chunk.bytes.assign(save.begin()+offset,save.begin()+end); outgoing_.push_back(std::move(chunk));
    }
  }
  void Emit(MessageType type, FlowEvent f) {
    if (outgoing_.size() >= 64) { Fail("Co-op control queue full. Return to the launcher."); return; }
    Message m; m.type=type; m.flow=f; outgoing_.push_back(std::move(m));
  }
  void Setup(SessionStart s, double now) {
    local_loaded_=remote_loaded_=local_finished_=remote_finished_=false;
    rejoin_requested_=false;
    view_.active=true; view_.start=s; view_.world={s.generation,s.level,s.rejoin ? s.world_start : s.start,s.completed};
    view_.completed=s.completed;
    if (s.rejoin) ++view_.rejoin_revision;
    view_.checkpoint=s.start; view_.policy=PolicyFor(s.level); view_.phase=FlowPhase::Loading;
    view_.status="Loading..."; deadline_=now;
  }
  void World(FlowEvent f, double now) {
    view_.reload=!COOP_MUT_IF("reload_mark") && reload_from_ && f.generation==reload_from_+1 &&
        f.level==view_.world.level && f.start==view_.checkpoint;
    reload_from_=0;
    view_.world=f; view_.completed=f.completed; view_.checkpoint=f.start;
    view_.policy=PolicyFor(f.level); view_.phase=FlowPhase::Loading;
    view_.status="Loading..."; deadline_=now;
    local_loaded_=remote_loaded_=local_finished_=remote_finished_=false;
    view_.partner_life = {};
  }
  bool Match(const FlowEvent& f) const { return f.generation==view_.world.generation && f.level==view_.world.level; }
  void Release() {
    view_.phase=FlowPhase::Playing;
    view_.status=view_.policy==ChapterPolicy::SoloRegroup ? "Solo section. Finish to regroup with partner." :
        view_.policy==ChapterPolicy::Observer && !view_.host ? "Observer section. Following host progress." : "";
  }
  void Advance(double now) {
    if (!view_.host || !next_ || !local_finished_ || !remote_finished_) return;
    auto f=*next_; next_.reset();
    if (const int chapter=FlowChapter(view_.world.level); chapter>=0 && chapter<41 && f.level!=view_.world.level)
      f.completed=view_.completed | (uint64_t(1)<<chapter);
    else f.completed=view_.completed;
    f.generation=view_.world.generation+1;
    World(f,now); Emit(MessageType::LevelChange,f);
  }
  void Handle(const Message& m, bool local, double now) {
    const bool authority = local == view_.host;
    if (m.type==MessageType::SessionStart) {
      if (local || view_.host || (view_.active && !m.start.rejoin) || !ValidSessionStart(m.start) || FlowChapter(m.start.level)!=m.start.chapter) return;
      // A repeated start from the same handshake cannot load twice.
      if (view_.active && m.start.rejoin && !view_.reconnecting && view_.phase==FlowPhase::Loading) return;
      outgoing_.clear(); view_.reconnecting=false; view_.reconnect_expired=false;
      Setup(m.start,now); view_.phase=FlowPhase::Preparing; view_.save.clear();
      view_.start_ready=m.start.save_bytes==0;
      if (view_.start_ready) view_.phase=FlowPhase::Loading;
      return;
    }
    if (!view_.active || view_.phase==FlowPhase::Failed) return;
    if (IsLifeMessage(m.type)) {
      if (view_.phase != FlowPhase::Playing || view_.policy != ChapterPolicy::Together) return;
      if (local) {
        if (!ValidPlayerLife(m.life) || !Match(m.life.world) || m.life.world.start != view_.world.start) return;
        if (outgoing_.size() >= 64) { Fail("Co-op control queue full. Return to the launcher."); return; }
        outgoing_.push_back(m);
      } else view_.partner_life.Accept(m,view_.world,now);
      return;
    }
    if (m.type==MessageType::SaveChunk) {
      if (local || view_.host || view_.start_ready || m.save_chunk.offset!=view_.save.size() ||
          m.save_chunk.bytes.empty() || m.save_chunk.bytes.size()>kSaveChunkBytes ||
          view_.save.size()+m.save_chunk.bytes.size()>view_.start.save_bytes) return;
      view_.save.insert(view_.save.end(),m.save_chunk.bytes.begin(),m.save_chunk.bytes.end());
      if (view_.save.size()==view_.start.save_bytes) {
        if (SessionSaveHash(view_.save)!=view_.start.save_hash) { Fail("Host save transfer failed. Return to the launcher."); return; }
        view_.start_ready=true; view_.phase=FlowPhase::Loading;
      }
      return;
    }
    if (!ValidFlowEvent(m.flow)) return;
    if (m.type==MessageType::RejoinRequest) {
      if (view_.host && !local && !guest_rejoining_ &&
          !(view_.phase==FlowPhase::Loading && view_.start.rejoin && view_.start.generation==view_.world.generation)) Rejoin(now);
      return;
    }
    if (view_.host && !local && m.type==MessageType::Loaded && guest_rejoining_ &&
        m.flow.generation==rejoin_world_.generation && m.flow.level==rejoin_world_.level &&
        m.flow.start==rejoin_world_.start && view_.phase==FlowPhase::Playing) {
      Emit(MessageType::LoadRelease,rejoin_world_); guest_rejoining_=false;
      view_.status=""; view_.partner_life={}; return;
    }
    if (m.type==MessageType::LevelChange) {
      if (!authority) return;
      if (local) {
        if (m.flow.generation!=view_.world.generation || view_.phase!=FlowPhase::Playing) return;
        // Independent checkpoint retries never become shared world changes.
        if (m.flow.level==view_.world.level && DeathRouteFor(view_.policy)==DeathRoute::LocalCheckpoint) return;
        next_=m.flow; local_finished_=true;
        if (view_.policy==ChapterPolicy::SoloRegroup && m.flow.level!=view_.world.level) {
          view_.phase=FlowPhase::Waiting; view_.status="Waiting for partner to finish this chapter...";
          deadline_=now; Emit(MessageType::ChapterFinished,view_.world);
        } else remote_finished_=true;
        Advance(now);
      } else if (view_.start_ready && m.flow.generation==view_.world.generation+1 &&
          (m.flow.completed & view_.completed)==view_.completed) World(m.flow,now);
      return;
    }
    if (!Match(m.flow)) return;
    switch (m.type) {
      case MessageType::Loaded:
        if (m.flow.start!=view_.world.start || view_.phase!=FlowPhase::Loading || !view_.start_ready) return;
        if (local) {
          if (local_loaded_) return;
          local_loaded_=true; view_.status="Waiting for partner..."; Emit(m.type,view_.world);
        }
        else remote_loaded_=true;
        if (view_.host && local_loaded_ && (remote_loaded_ || view_.reconnecting || rejoin_deferred_)) {
          if (remote_loaded_) Emit(MessageType::LoadRelease,view_.world);
          Release();
        }
        break;
      case MessageType::LoadRelease:
        if (!local && authority && local_loaded_ && m.flow.start==view_.world.start && view_.phase==FlowPhase::Loading) Release();
        break;
      case MessageType::ChapterFinished:
        if (view_.policy!=ChapterPolicy::SoloRegroup) return;
        if (local) { if (local_finished_) return; local_finished_=true; Emit(m.type,view_.world); }
        else remote_finished_=true;
        if (local_finished_) { view_.phase=FlowPhase::Waiting; view_.status="Waiting for partner to finish this chapter..."; deadline_=now; }
        Advance(now); break;
      case MessageType::Checkpoint:
        if (!authority || m.flow.start==view_.checkpoint) return;
        view_.checkpoint=m.flow.start;
        if (local) Emit(m.type,m.flow);
        break;
      case MessageType::ReloadRequest:
        // Also reject a delayed/accidental peer request while a solo finisher
        // waits. It must not cancel completion or restart the living partner.
        if (DeathRouteFor(view_.policy)==DeathRoute::LocalCheckpoint) return;
        reload_from_=view_.world.generation;
        if (local) Emit(m.type,view_.world);
        if (view_.host && (view_.phase==FlowPhase::Playing || view_.phase==FlowPhase::Waiting)) {
          auto f=view_.world; ++f.generation; f.start=view_.checkpoint;
          World(f,now); Emit(MessageType::LevelChange,f);
        }
        break;
      case MessageType::FlowAbort:
        if (local) Emit(m.type,view_.world);
        Fail("Co-op session stopped. Return to the launcher."); break;
      default: break;
    }
  }
  FlowView view_;
  double deadline_=0;
  bool local_loaded_=false,remote_loaded_=false,local_finished_=false,remote_finished_=false;
  std::optional<FlowEvent> next_;
  std::deque<Message> outgoing_;
  std::optional<SessionStart> checkpoint_state_;
  uint32_t reload_from_=0; // generation a coordinated ReloadRequest restarts
  uint32_t reconnects_=0;
  bool guest_rejoining_=false;
  bool rejoin_requested_=false;
  bool rejoin_deferred_=false;
  FlowEvent rejoin_world_;
  double rejoin_deadline_=0;
};

class FlowMailbox {
 public:
  void Disconnect() {
    std::lock_guard lock(mutex_);
    // Keep the failure visible to a running game, but discard work from this
    // connection. The next session must learn its own role and start afresh.
    flow_.Fail("Partner disconnected. Return to the launcher.");
    pending_.reset(); local_.clear(); initialized_=false;
    // A plan both sides agreed on stays for the hand over: the link ends as both restart.
    if (plan_.Phase()!=handover::PlanPhase::Ready) plan_.Reset();
    plan_pending_.reset();
  }
  // C80: the start plan before the Play restart (coop_handover.h). Host only; sent once connected.
  bool BeginPlan(const StartPlan& plan) {
    std::lock_guard lock(mutex_);
    if (plan_pending_ || plan_.Phase()!=handover::PlanPhase::Idle || (initialized_ && flow_.View().active)) return false;
    plan_pending_=plan; return true;
  }
  handover::PlanPhase PlanState(StartPlan* plan=nullptr, const char** failure=nullptr) const {
    std::lock_guard lock(mutex_);
    if (plan) *plan=plan_.Plan();
    if (failure) *failure=plan_.Failure();
    return plan_pending_ ? handover::PlanPhase::WaitingAck : plan_.Phase();
  }
  bool Begin(SessionStart start, std::vector<uint8_t> save) {
    std::lock_guard lock(mutex_); if (pending_ || (initialized_ && flow_.View().active)) return false;
    pending_=std::pair{start,std::move(save)}; return true;
  }
  bool Submit(Message m) {
    std::lock_guard lock(mutex_); if (local_.size()>=64) return false;
    local_.push_back(std::move(m)); return true;
  }
  FlowView Snapshot() const { std::lock_guard lock(mutex_); return flow_.View(); }
  void PublishRejoinState(const SessionStart& s) { std::lock_guard lock(mutex_); flow_.PublishRejoinState(s); }
  void Exchange(CoopSession& session, double now) {
    std::lock_guard lock(mutex_);
    if (!initialized_) { flow_=SessionFlow(session.SessionRole()==Role::Host); initialized_=true; }
    plan_.Tick(now,session.State());
    if (session.State()==SessionState::Ended && !session.RejoinEnabled()) {
      flow_.Fail("Partner left the session. Return to the launcher."); return;
    }
    flow_.Connection(session.State(),session.ReconnectCount(),now);
    if (session.State()==SessionState::Ended) return;
    if (session.State()!=SessionState::Connected) {
      // The host's checkpoints still advance while the partner is absent.
      // Keep them in the authoritative flow before building the rejoin start.
      if (session.SessionRole()==Role::Host) for (const auto& m : local_)
        if (!IsLifeMessage(m.type)) flow_.Local(m,now);
      local_.clear(); flow_.Tick(now); return;
    }
    if (pending_) { flow_.Begin(pending_->first,std::move(pending_->second),now); pending_.reset(); }
    if (plan_pending_) {
      if (session.SessionRole()!=Role::Host || !plan_.Begin(*plan_pending_,now)) plan_.Reset();
      plan_pending_.reset();
    }
    for (const auto& m : session.TakeFlow()) {
      if (IsPlanMessage(m.type)) plan_.Remote(m,now);
      else flow_.Remote(m,now);
    }
    while (!plan_.Outgoing().empty() && session.QueueFlow(plan_.Outgoing().front())) plan_.Sent();
    for (const auto& m : local_) flow_.Local(m,now);
    local_.clear(); flow_.Tick(now);
    while (!flow_.Outgoing().empty() && session.QueueFlow(flow_.Outgoing().front())) flow_.Sent();
  }
 private:
  mutable std::mutex mutex_;
  SessionFlow flow_;
  bool initialized_=false;
  std::optional<std::pair<SessionStart,std::vector<uint8_t>>> pending_;
  std::vector<Message> local_;
  handover::PlanExchange plan_;
  std::optional<StartPlan> plan_pending_;
};
}  // namespace kk::coop
