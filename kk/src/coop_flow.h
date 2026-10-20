#pragma once

#include "coop_session.h"
#include "coop_revive.h"
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
};
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
// world. Track actual native checkpoints/loads separately and try each newer
// host checkpoint at most once. A failed private catch-up must never loop.
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
    if (!Available() && !(!attempted_ && host_==world_ && (desync_recovery || (checkpoint_offer && offered_until_>=0)))) return false;
    attempted_=true; return true;
  }
  uint32_t Target() const { return host_; }
  uint32_t WorldCheckpoint() const { return world_; }
 private:
  uint32_t generation_=0,world_=0,host_=0;
  double offered_until_=-1;
  bool attempted_=false;
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
  bool Begin(SessionStart start, std::vector<uint8_t> save, double now) {
    if (!view_.host || view_.active || !ValidSessionStart(start) || start.save_bytes != save.size() ||
        start.save_hash != SessionSaveHash(save) || FlowChapter(start.level) != start.chapter) return false;
    Message m; m.type = MessageType::SessionStart; m.start = start; outgoing_.push_back(m);
    for (uint32_t offset=0; offset<save.size(); offset += kSaveChunkBytes) {
      Message chunk; chunk.type = MessageType::SaveChunk; chunk.save_chunk.offset = offset;
      const size_t end = std::min(save.size(), size_t(offset+kSaveChunkBytes));
      chunk.save_chunk.bytes.assign(save.begin()+offset, save.begin()+end); outgoing_.push_back(std::move(chunk));
    }
    Setup(start, now); view_.save = std::move(save); view_.start_ready = true; return true;
  }
  void Local(Message m, double now) { Handle(m, true, now); }
  void Remote(const Message& m, double now) { Handle(m, false, now); }
  void Tick(double now) {
    if (view_.active && (view_.phase==FlowPhase::Preparing || view_.phase==FlowPhase::Loading) && now-deadline_ >= 120) {
      Emit(MessageType::FlowAbort, view_.world); Fail("Co-op timed out waiting for partner. Return to the launcher.");
    }
  }
 private:
  void Emit(MessageType type, FlowEvent f) {
    if (outgoing_.size() >= 64) { Fail("Co-op control queue full. Return to the launcher."); return; }
    Message m; m.type=type; m.flow=f; outgoing_.push_back(std::move(m));
  }
  void Setup(SessionStart s, double now) {
    view_.active=true; view_.start=s; view_.world={1,s.level,s.start,0};
    view_.checkpoint=s.start; view_.policy=PolicyFor(s.level); view_.phase=FlowPhase::Loading;
    view_.status="Loading..."; deadline_=now;
  }
  void World(FlowEvent f, double now) {
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
      if (local || view_.host || view_.active || !ValidSessionStart(m.start) || FlowChapter(m.start.level)!=m.start.chapter) return;
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
        if (view_.host && local_loaded_ && remote_loaded_) { Emit(MessageType::LoadRelease,view_.world); Release(); }
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
};

class FlowMailbox {
 public:
  void Disconnect() {
    std::lock_guard lock(mutex_);
    // Keep the failure visible to a running game, but discard work from this
    // connection. The next session must learn its own role and start afresh.
    flow_.Fail("Partner disconnected. Return to the launcher.");
    pending_.reset(); local_.clear(); initialized_=false;
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
  void Exchange(CoopSession& session, double now) {
    std::lock_guard lock(mutex_);
    if (!initialized_) { flow_=SessionFlow(session.SessionRole()==Role::Host); initialized_=true; }
    if (session.State()==SessionState::Ended) { flow_.Fail("Partner disconnected. Return to the launcher."); return; }
    if (session.State()!=SessionState::Connected) { flow_.Tick(now); return; }
    if (pending_) { flow_.Begin(pending_->first,std::move(pending_->second),now); pending_.reset(); }
    for (const auto& m : session.TakeFlow()) flow_.Remote(m,now);
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
};
}  // namespace kk::coop
