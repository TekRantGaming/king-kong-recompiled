#pragma once
#include "coop_enemies.h"
#include <map>

namespace kk::coop::story {
inline bool Observing(uint32_t generation, uint32_t level, bool playing) {
  return generation && level && playing;
}
inline bool LoopEvent(const StoryEvent& e) {
  return e.phase==StoryPhase::Advance || e.phase==StoryPhase::Finished;
}
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
    return e.run==expected.run && e.record==expected.record ? Result::Match : Result::Mismatch;
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
struct Binding { uint64_t source=0, script=0; uint32_t actor=0, ai=0, vars=0; };
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
  void Reset(uint32_t generation, uint32_t level) { *this={}; generation_=generation; level_=level; }
  bool Push(const StoryEvent& e, double now) {
    if (!ValidStoryEvent(e) || !std::isfinite(now) || now<0) return false;
    if (e.generation!=generation_ || e.level!=level_ || e.number<next_) return false;
    if (behind_) return false;
    if (const auto it=pending_.find(e.number); it!=pending_.end()) {
      if (it->second.event!=e) Refuse();
      return false;
    }
    if (e.number-next_>=kStoryQueue || pending_.size()>=kStoryQueue) { Refuse(); return false; }
    pending_.emplace(e.number,Entry{e,now}); return true;
  }
  const StoryEvent* Front() const {
    const auto it=pending_.find(next_);
    return !behind_ && it!=pending_.end() ? &it->second.event : nullptr;
  }
  void Applied() {
    pending_.erase(next_);
    if (next_==UINT32_MAX) Refuse(); else ++next_;
  }
  void Tick(double now) {
    for (const auto& [number,e] : pending_) if (!std::isfinite(now) || now<e.received || now-e.received>5) { Refuse(); break; }
  }
  // Recovery is a guest property, including observation errors before load.
  void Refuse(bool host=false) { if (!host) { behind_=true; pending_.clear(); } }
  bool Behind() const { return behind_; }
  uint32_t Next() const { return next_; }
 private:
  struct Entry { StoryEvent event; double received=0; };
  uint32_t generation_=0, level_=0, next_=1;
  bool behind_=false;
  std::map<uint32_t,Entry> pending_;
};
} // namespace kk::coop::story
