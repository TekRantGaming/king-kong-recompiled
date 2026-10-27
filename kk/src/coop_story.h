#pragma once
#include "coop_enemies.h"
#include "coop_mut.h"
#include <deque>
#include <map>

namespace kk::coop::story {
inline bool Observing(uint32_t generation, uint32_t level, bool playing) {
  return generation && level && playing;
}
inline bool LoopEvent(const StoryEvent& e) {
  return e.phase==StoryPhase::Advance || e.phase==StoryPhase::Finished;
}
inline bool RetirableWait(uint32_t callback) {
  return callback==0x82501D50 || callback==0x82501A30 || callback==0x8250B9D8 || callback==0x82449CD0;
}
inline bool NativeStopMatches(const StoryEvent& event,uint32_t generation,uint32_t level,
    uint64_t source,uint64_t script,uint32_t run,uint32_t record,uint32_t callback) {
  return event.phase==StoryPhase::Stopped && ValidStoryEvent(event) &&
      event.generation==generation && event.level==level && event.source==source && event.script==script &&
      event.run==run && event.record==record && RetirableWait(callback);
}
// 8271D1D8 and 826F9E70 initialize a native program's state word to -1.
// SSDetect treats only 1 as armed. Starting replaces that word with the new
// program's initial state, so its final value need not be zero.
inline bool DetectIdle(uint32_t state) { return state==0 || state==UINT32_MAX; }
inline bool DetectArmed(uint32_t before,uint32_t after) { return DetectIdle(before) && after==1; }
inline bool DetectStarted(uint32_t before,uint32_t result,bool loop_program) {
  return before==1 && result==5 && loop_program;
}
// Only reliable host activation evidence can arm a shared Jack-volume detector.
// Never skip a missing activation or infer one from an idle checksum baseline.
struct ActivationGate {
  uint32_t armed=0;
  void Observe(const StoryEvent& e) {
    if (e.phase==StoryPhase::Armed || e.phase==StoryPhase::Started || e.phase==StoryPhase::Advance ||
        e.phase==StoryPhase::Finished || e.phase==StoryPhase::SnapshotLoop || e.phase==StoryPhase::SnapshotFinished)
      armed=std::max(armed,e.run);
  }
  bool CanArm(uint32_t local) const { return local!=UINT32_MAX && armed==local+1; }
};
// A loaded record is already shared, including scripts armed or looping before
// our hooks saw them. Baseline never emits a story transition.
struct Cursor {
  uint32_t run=0, record=0;
  void Baseline(uint32_t index) { run=1; record=index; }
  bool ObserveLoop(uint32_t before, uint32_t after) {
    const bool observed=run && before==record && before<=after && after<=kStoryRecords;
    if (!run) run=1;
    record=after;
    return observed;
  }
};
// Native scripts may reach a host hint before or after its packet arrives.
// Runs order activations; record numbers only order work within the same run.
struct Progress : Cursor {
  uint32_t started=0, finished=0;
  bool Reached(const StoryEvent& e) const {
    if (run!=e.run) return run>e.run;
    switch (e.phase) {
      case StoryPhase::Armed: return true;
      case StoryPhase::Started: return started>=e.run;
      case StoryPhase::Advance: return record>e.record;
      case StoryPhase::Finished: return finished>=e.run;
      case StoryPhase::SnapshotLoop: return started>=e.run && record>=e.record;
      case StoryPhase::SnapshotFinished: return finished>=e.run;
      default: return false;
    }
  }
};
class Hints {
 public:
  void Remember(const StoryEvent& e) {
    // Retain observed host boundaries. Coalesce old evidence under pressure.
    if (pending_.size()==kStoryQueue) pending_.pop_front();
    pending_.push_back(e);
  }
  template<class F> void Converge(const Progress& p,F note) {
    for (auto it=pending_.begin();it!=pending_.end();)
      if (p.Reached(*it)) { note(*it); it=pending_.erase(it); } else ++it;
  }
  std::size_t Pending() const { return pending_.size(); }
  bool Passed(uint32_t run,uint32_t record) const {
    for (const auto& e : pending_) if (e.run==run &&
        ((e.phase==StoryPhase::Advance && e.record==record) ||
         ((e.phase==StoryPhase::Finished || e.phase==StoryPhase::SnapshotLoop || e.phase==StoryPhase::SnapshotFinished) && e.record>record))) return true;
    return false;
  }
 private:
  std::deque<StoryEvent> pending_;
};
// Only repeated checksum evidence of a stationary, behind guest counts as
// divergence. Host progress, guest-ahead samples and a lost reply do not count.
class Lag {
 public:
  enum class Stage { Clear, Waiting, Move, AfterMove, Reload };
  Stage Sample(const Cursor& authority,const Cursor& guest,double now) {
    if (!std::isfinite(now) || now<0) return stage_;
    const bool behind=guest.run<authority.run ||
        (guest.run==authority.run && guest.record<authority.record);
    if (!behind) { *this={}; return stage_; }
    const bool progressed=guest.run>local_.run || (guest.run==local_.run && guest.record>local_.record);
    if (stage_==Stage::Clear || progressed || guest.run<local_.run ||
        (guest.run==local_.run && guest.record<local_.record) || now<sampled_ || now-sampled_>10) {
      // A move can advance a few native records then hit another NPC wait.
      // Progress renews the grace period but does not forget that soft fix.
      if (guest.run!=local_.run || guest.record<local_.record || now<sampled_ || now-sampled_>10) moved_=false;
      stage_=moved_ ? Stage::AfterMove : Stage::Waiting; since_=now;
    }
    local_=guest; sampled_=now;
    if (stage_==Stage::Waiting && now-since_>20) stage_=Stage::Move;
    if (stage_==Stage::AfterMove && now-since_>20) stage_=Stage::Reload;
    return stage_;
  }
  void Moved(double now) {
    if (stage_==Stage::Move && std::isfinite(now) && now>=sampled_) { moved_=true; stage_=Stage::AfterMove; since_=now; }
  }
  Stage State(double now) const {
    return std::isfinite(now) && now>=sampled_ && now-sampled_<=10 ? stage_ : Stage::Clear;
  }
 private:
  Stage stage_=Stage::Clear;
  Cursor local_;
  double since_=0, sampled_=0;
  bool moved_=false;
};
// A reliable request is sent once per activation, including while it is pending
// or after authority refuses it. Only a new run or a new world permits another.
struct RequestLatch {
  uint32_t run=0;
  bool Begin(uint32_t next_run) {
    if (!next_run || next_run<=run) return false;
    run=next_run; return true;
  }
};
// Match replies against the captured boundary, never the host's later live
// record. Missing replies expire harmlessly; only a compared state can differ.
class Checksums {
 public:
  enum class Result { Ignored, Match, Mismatch };
  bool Remember(const StoryEvent& e, double now) {
    Expire(now);
    if (pending_.size()>=kStoryQueue/2 || e.phase!=StoryPhase::Checksum) return false;
    return pending_.emplace(e.number,Sample{e,now}).second;
  }
  Result Compare(const StoryEvent& e) {
    const auto it=pending_.find(e.number);
    if (it==pending_.end()) return Result::Ignored;
    const auto expected=it->second.event;
    if (e.phase!=StoryPhase::Checksum || e.generation!=expected.generation || e.level!=expected.level ||
        e.source!=expected.source || e.script!=expected.script) return Result::Ignored;
    pending_.erase(it);
    return COOP_MUT_IF("digest_compare") || (e.run==expected.run && e.record==expected.record) ? Result::Match : Result::Mismatch;
  }
  void Expire(double now) {
    for (auto it=pending_.begin();it!=pending_.end();)
      if (now-it->second.time>10) it=pending_.erase(it); else ++it;
  }
 private:
  struct Sample { StoryEvent event; double time; };
  std::map<uint32_t,Sample> pending_;
};
// Same explicit quantization as C18. Hash only immutable evidence, not a live
// pointer, latest pose, mutable payload, or world-table enumeration ordinal.
inline uint64_t Source(uint64_t script, float x, float y, float z) {
  const auto position = enemies::AuthoredKey(0x53544f52, x, y, z);
  return script && position ? enemies::Mix(position, script) : 0;
}
inline uint64_t FileSource(uint64_t script,uint32_t key) {
  const auto source=enemies::FileAuthoredKey(0x53544f52,key);
  return script && source ? enemies::Mix(source,script):0;
}
struct Binding { uint64_t source=0, script=0; uint32_t actor=0, ai=0, vars=0; };
inline bool CanReplaySnapshotPrefix(bool looping,uint32_t native_index,const StoryEvent& target) {
  return (target.phase==StoryPhase::SnapshotLoop || target.phase==StoryPhase::SnapshotFinished) &&
      target.record<=kStoryRecords && native_index<=target.record && (looping || native_index==0);
}
// A newer native checkpoint can omit a completed prefix from scheduled AI,
// resetting its record to zero. This covers history, never executes/skips a
// callback. Active or subsequently scheduled programs cannot use this baseline.
inline bool CanCoverCheckpointPrefix(bool omitted_at_load,bool omitted_now,bool scheduled,
    bool looping,uint32_t native_index,uint32_t local_run,bool terminator,const StoryEvent& end) {
  return omitted_at_load && omitted_now && !scheduled && !looping &&
      native_index==0 && local_run<=end.run && terminator && ValidStoryEvent(end) &&
      (end.phase==StoryPhase::Finished || end.phase==StoryPhase::SnapshotFinished);
}
inline bool CheckpointPrefixCovers(const StoryEvent& end,const StoryEvent& event) {
  return end.generation==event.generation && end.level==event.level && end.source==event.source &&
      end.script==event.script && end.run==event.run && event.record<=end.record;
}
// A private reload accepts one complete, token-scoped host cursor census.
// Partial/duplicate/mixed censuses never modify native scripts.
class Snapshot {
 public:
  void Request(uint32_t generation,uint32_t level,uint64_t token) {
    *this={}; generation_=generation; level_=level; token_=token;
  }
  bool Begin(const StoryEvent& e) {
    if (!Marker(e,StoryPhase::SnapshotBegin) || begun_) return false;
    expected_=uint32_t(e.script-1); begun_=true; return true;
  }
  bool Add(const StoryEvent& e) {
    if (!begun_ || complete_ || !Scope(e) || !StorySnapshotEntry(e.phase) || entries_.size()>=expected_) return false;
    return entries_.emplace(std::pair(e.source,e.script),e).second;
  }
  bool End(const StoryEvent& e) {
    if (!begun_ || complete_ || !Marker(e,StoryPhase::SnapshotEnd) ||
        e.script!=uint64_t(expected_)+1 || entries_.size()!=expected_) return false;
    complete_=true; return true;
  }
  bool Complete() const { return complete_; }
  bool Receiving() const { return begun_ && !complete_; }
  const auto& Entries() const { return entries_; }
 private:
  bool Scope(const StoryEvent& e) const { return ValidStoryEvent(e) && e.generation==generation_ && e.level==level_; }
  bool Marker(const StoryEvent& e,StoryPhase phase) const { return Scope(e) && token_ && e.phase==phase && e.source==token_; }
  uint32_t generation_=0,level_=0,expected_=0;
  uint64_t token_=0;
  bool begun_=false,complete_=false;
  std::map<std::pair<uint64_t,uint64_t>,StoryEvent> entries_;
};
inline const Binding* Match(uint64_t source, uint64_t script, std::span<const Binding> bindings) {
  const Binding* found=nullptr;
  for (const auto& b : bindings) if (b.source==source && b.script==script) {
    if (found || !b.actor || !b.ai || !b.vars) return nullptr;
    found=&b;
  }
  if (found) for (const auto& b : bindings) if (&b!=found &&
      (b.actor==found->actor || b.ai==found->ai || b.vars==found->vars)) return nullptr;
  return found;
}

// Transport reliability does not imply cross-script native scheduling order.
// The guest consumes only Front(), including when callbacks run in another order.
class Ordered {
 public:
  enum class Failure { None, Conflict, Window, Gap, ApplyTimeout, Clock, Overflow };
  struct Diagnostic {
    Failure reason=Failure::None;
    uint32_t expected=0;
    StoryEvent received;
  };
  static const char* Reason(Failure reason) {
    switch (reason) {
      case Failure::Conflict: return "conflicting event";
      case Failure::Window: return "reorder window exceeded";
      case Failure::Gap: return "missing event number";
      case Failure::ApplyTimeout: return "front event apply timeout";
      case Failure::Clock: return "invalid ordering clock";
      case Failure::Overflow: return "event number overflow";
      default: return "no ordering failure";
    }
  }
  void Reset(uint32_t generation, uint32_t level) { *this={}; generation_=generation; level_=level; }
  bool ResumeAfter(const StoryEvent& boundary) {
    if (!ValidStoryEvent(boundary) || boundary.phase!=StoryPhase::SnapshotBegin || boundary.number==UINT32_MAX) return false;
    Reset(boundary.generation,boundary.level); next_=boundary.number+1; return true;
  }
  bool Push(const StoryEvent& e, double now) {
    if (!ValidStoryEvent(e) || !std::isfinite(now) || now<0) return false;
    if (!COOP_MUT_IF("story_world") && (e.generation!=generation_ || e.level!=level_)) return false;
    if (e.number<next_) {
      if (!COOP_MUT_IF("story_duplicate")) return false;
      next_=e.number; // twin reopens a previously applied event
    }
    if (behind_) return false;
    if (const auto it=pending_.find(e.number); it!=pending_.end()) {
      if (it->second.event!=e) Fail(Failure::Conflict,e);
      return false;
    }
    if (e.number-next_>=kStoryQueue || pending_.size()>=kStoryQueue) { Fail(Failure::Window,e); return false; }
    if (pending_.empty() || e.number==next_) blocked_since_=now;
    pending_.emplace(e.number,Entry{e,now}); return true;
  }
  const StoryEvent* Front() const {
    const auto it=pending_.find(next_);
    return !behind_ && it!=pending_.end() ? &it->second.event : nullptr;
  }
  void Applied(double now) {
    const auto* front=Front();
    if (!front) return;
    const auto applied=*front;
    if (!std::isfinite(now) || now<blocked_since_) { Fail(Failure::Clock,applied); return; }
    pending_.erase(next_);
    if (next_==UINT32_MAX) Fail(Failure::Overflow,applied); else ++next_;
    // Deadline measures lack of ordered progress. Events for another native
    // script can have spent several seconds waiting behind the previous head.
    blocked_since_=now;
  }
  void Tick(double now) {
    if (behind_ || pending_.empty()) return;
    const auto& e=pending_.begin()->second.event;
    if (!std::isfinite(now) || now<blocked_since_) Fail(Failure::Clock,e);
    else if (now-blocked_since_>5) Fail(Front() ? Failure::ApplyTimeout : Failure::Gap,e);
  }
  // Recovery is a guest property, including observation errors before load.
  void Refuse(bool host=false) { if (!host) { behind_=true; pending_.clear(); } }
  bool Behind() const { return behind_; }
  uint32_t Next() const { return next_; }
  const Diagnostic& Error() const { return error_; }
 private:
  void Fail(Failure reason,const StoryEvent& e) { error_={reason,next_,e}; Refuse(); }
  struct Entry { StoryEvent event; double received=0; };
  uint32_t generation_=0, level_=0, next_=1;
  bool behind_=false;
  double blocked_since_=0;
  Diagnostic error_;
  std::map<uint32_t,Entry> pending_;
};
} // namespace kk::coop::story
