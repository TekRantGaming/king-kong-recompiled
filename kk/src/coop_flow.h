#pragma once

#include "coop_session.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace kk::coop {
enum class ChapterPolicy { Together, SoloRegroup, Observer };
struct ChapterRule { uint32_t level; ChapterPolicy policy; bool uncertain; };
// C01 provisional classifications. No row has runtime certification. E rows use
// the final fallback, a status screen, rather than inventing a camera stream.
inline constexpr std::array<ChapterRule, 42> kChapterRules{{
  {100,ChapterPolicy::SoloRegroup,true}, {101,ChapterPolicy::Together,true},
  {201,ChapterPolicy::Together,true}, {203,ChapterPolicy::Together,true},
  {204,ChapterPolicy::Together,true}, {301,ChapterPolicy::Together,true},
  {302,ChapterPolicy::Together,true}, {303,ChapterPolicy::Together,true},
  {305,ChapterPolicy::Together,true}, {306,ChapterPolicy::Together,true},
  {304,ChapterPolicy::SoloRegroup,true}, {401,ChapterPolicy::Together,true},
  {402,ChapterPolicy::Together,true}, {403,ChapterPolicy::Together,true},
  {404,ChapterPolicy::Together,true}, {501,ChapterPolicy::SoloRegroup,true},
  {502,ChapterPolicy::SoloRegroup,true}, {504,ChapterPolicy::SoloRegroup,true},
  {702,ChapterPolicy::Together,true}, {705,ChapterPolicy::SoloRegroup,true},
  {901,ChapterPolicy::Together,true}, {1002,ChapterPolicy::Together,true},
  {1101,ChapterPolicy::Together,true}, {1200,ChapterPolicy::Together,true},
  {1201,ChapterPolicy::Together,true}, {703,ChapterPolicy::Together,true},
  {1401,ChapterPolicy::Together,true}, {1402,ChapterPolicy::Together,true},
  {1501,ChapterPolicy::Together,true}, {1701,ChapterPolicy::Together,true},
  {1702,ChapterPolicy::Together,true}, {1703,ChapterPolicy::SoloRegroup,true},
  {1801,ChapterPolicy::Together,true}, {1803,ChapterPolicy::Together,true},
  {1804,ChapterPolicy::Together,true}, {1805,ChapterPolicy::Together,true},
  {1901,ChapterPolicy::SoloRegroup,true}, {1902,ChapterPolicy::SoloRegroup,true},
  {2001,ChapterPolicy::SoloRegroup,true}, {2003,ChapterPolicy::SoloRegroup,true},
  {2004,ChapterPolicy::Observer,true}, {7,ChapterPolicy::Observer,true}
}};
inline int FlowChapter(uint32_t level) {
  for (size_t i=0; i<kChapterRules.size(); ++i) if (kChapterRules[i].level == level) return int(i);
  return -1;
}
inline ChapterPolicy PolicyFor(uint32_t level) {
  const int c = FlowChapter(level);
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
};
// Continuous evidence only; loading, stale poses or solo policy reset the timer.
class BehindRule {
 public:
  bool Tick(double now, const std::optional<PlayerPose>& local, const std::optional<PlayerPose>& host) {
    if (!local || !host || now-local->time > .5 || !std::isfinite(now)) { since_.reset(); return false; }
    const bool far = local->level != host->level || std::hypot(local->x-host->x, local->y-host->y, local->z-host->z) > 100.f;
    if (!far) { since_.reset(); return false; }
    if (!since_ || now < *since_) since_ = now;
    if (now-*since_ < 8) return false;
    since_.reset(); return true;
  }
 private:
  std::optional<double> since_;
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
    view_.status="Loading co-op. Waiting for partner..."; deadline_=now;
  }
  void World(FlowEvent f, double now) {
    view_.world=f; view_.completed=f.completed; view_.checkpoint=f.start;
    view_.policy=PolicyFor(f.level); view_.phase=FlowPhase::Loading;
    view_.status="Loading co-op. Waiting for partner..."; deadline_=now;
    local_loaded_=remote_loaded_=local_finished_=remote_finished_=false;
  }
  bool Match(const FlowEvent& f) const { return f.generation==view_.world.generation && f.level==view_.world.level; }
  void Release() {
    view_.phase=FlowPhase::Playing;
    view_.status=view_.policy==ChapterPolicy::SoloRegroup ? "Solo section. Finish to regroup with partner." :
        view_.policy==ChapterPolicy::Observer ? "Observer section. Following host progress." : "";
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
        if (local) { if (local_loaded_) return; local_loaded_=true; Emit(m.type,view_.world); }
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
        if (local) Emit(m.type,view_.world);
        if (view_.host && (view_.phase==FlowPhase::Playing || view_.phase==FlowPhase::Waiting)) {
          auto f=view_.world; ++f.generation; f.start=view_.checkpoint;
          World(f,now); Emit(MessageType::LevelChange,f);
          view_.status="Co-op recovery: both players reload the host checkpoint.";
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
  void Disconnect() { std::lock_guard lock(mutex_); flow_.Fail("Partner disconnected. Return to the launcher."); }
  bool Begin(SessionStart start, std::vector<uint8_t> save) {
    std::lock_guard lock(mutex_); if (pending_ || flow_.View().active) return false;
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
