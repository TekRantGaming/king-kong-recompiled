#pragma once
#include "coop_enemies.h"
#include "coop_pose.h"
#include "coop_stimulus_debug.h"
#include <map>
#include <set>
#include <bit>

namespace kk::coop::objects {
constexpr size_t kQueue=kObjectQueue, kCensus=kObjectCensus;
// The enqueue result must be NEW and name this exact local shooter's record.
// Another environmental hit on the same obstacle cannot borrow its provenance.
template<class Read> bool StrikeRecord(Read read,uint32_t output,uint64_t previous,
    uint32_t shooter,uint32_t target,uint32_t flags,uint32_t magnitude,float seconds=0) {
  if (!PoseAddress(output,8) || magnitude>100000 ||
      (uint64_t(read(output))<<32|read(output+4))==previous) return false;
  const auto r=stimulus_debug::ReadHandle(read,output);
  return r.valid && (r.id>>24)==4 && r.source==shooter && r.target==target &&
      read(r.address+96)==flags && read(r.address+100)==magnitude &&
      read(r.address+4)==std::bit_cast<uint32_t>(seconds);
}
// Same native loaded-file join used by dev_sample.cpp: key -> file index ->
// loaded address. Authored actor keys survive heap relocation and moving doors.
// Refuse aliases instead of accepting the last row for a duplicated address.
template<class Read> std::map<uint32_t,uint32_t> FileKeys(Read read) {
  std::map<uint32_t,uint32_t> indices,out;
  std::set<uint32_t> duplicate_indices,duplicate_addresses;
  size_t total=0;
  for (uint32_t bucket=0;bucket<256;++bucket) {
    const auto list=read(0x82C4EDC0+bucket*16),count=read(0x82C4EDC4+bucket*16);
    if (!count) continue;
    if (count>100000 || total+count>100000 || !PoseAddress(list,count*8)) return {};
    total+=count;
    for (uint32_t i=0;i<count;++i) {
      const auto index=read(list+8*i),address=read(list+8*i+4);
      if (!PoseAddress(address)) continue;
      const auto [it,added]=indices.emplace(index,address);
      if (!added && it->second!=address) duplicate_indices.insert(index);
    }
  }
  const auto list=read(0x82C4ED94),count=read(0x82C4ED98);
  if (!count || count>1000000 || !PoseAddress(list,count*8)) return {};
  for (uint32_t i=0;i<count;++i) {
    const auto key=read(list+8*i),index=read(list+8*i+4);
    const auto it=indices.find(index);
    if (!key || it==indices.end() || duplicate_indices.contains(index)) continue;
    const auto [entry,added]=out.emplace(it->second,key);
    if (!added && entry->second!=key) duplicate_addresses.insert(it->second);
  }
  for (const auto address : duplicate_addresses) out.erase(address);
  return out;
}
inline uint64_t AssetSource(ObjectKind kind,uint32_t key) {
  if (!key || (kind!=ObjectKind::Breakable && kind!=ObjectKind::Door)) return 0;
  const auto hash=enemies::Mix(enemies::Mix(14695981039346656037ull,0x4f424a4b),uint64_t(kind)<<32|key);
  return hash ? hash : 1;
}
struct Binding {
  uint32_t actor=0,ai=0,vars=0,matrix=0;
  uint64_t source=0;
  ObjectKind kind=ObjectKind::Breakable;
  auto operator<=>(const Binding&) const = default;
};
// Ambiguity does not disappear when one duplicate leaves the registry. Never
// recycle its authored ID into the survivor or a later actor at that position.
// A private native load clears local addresses before rebinding host outcomes.
class Identities {
 public:
  bool Record(const Binding& b) {
    if (!b.source || !b.actor || !b.ai || !b.vars || !b.matrix) return false;
    const auto it=first_.find(b.source);
    if (it==first_.end()) {
      if (first_.size()>=kCensus) return false;
      first_.emplace(b.source,b);
    } else if (it->second!=b) ambiguous_.insert(b.source);
    return Allows(b.kind,b.source);
  }
  bool Allows(ObjectKind kind,uint64_t source) const {
    const auto it=first_.find(source);
    return it!=first_.end() && it->second.kind==kind && !ambiguous_.contains(source);
  }
  void Clear() { first_.clear(); ambiguous_.clear(); }
 private:
  std::map<uint64_t,Binding> first_;
  std::set<uint64_t> ambiguous_;
};
template<class T> T* Match(uint64_t source,ObjectKind kind,std::map<uint32_t,T>& bindings) {
  T* found=nullptr;
  for (auto& [actor,o] : bindings) if (o.source==source && o.kind==kind) {
    if (found || !o.actor || !o.ai || !o.vars || !o.matrix) return nullptr;
    found=&o;
  }
  if (found) for (auto& [actor,o] : bindings) if (&o!=found &&
      (o.source==found->source || o.actor==found->actor || o.ai==found->ai || o.vars==found->vars || o.matrix==found->matrix)) return nullptr;
  return found;
}
inline uint64_t Source(ObjectKind kind,float x,float y,float z,uint64_t shape) {
  const auto position=enemies::AuthoredKey(0x4f424a00u+uint16_t(kind),x,y,z);
  return position && shape ? enemies::Mix(position,shape) : 0;
}
// Authority is absolute, not a delta: duplicate packets and private native
// reloads cannot apply a strike twice. Requests have a separate replay cursor.
class Replica {
 public:
  bool Observe(const ObjectState& s) {
    if (!ValidObjectState(s)) return false;
    const auto key=std::pair(s.kind,s.source);
    const auto it=states_.find(key);
    if (it!=states_.end() && (it->second.generation!=s.generation || it->second.level!=s.level ||
        it->second.number>=s.number)) return false;
    if (states_.size()>=kCensus && it==states_.end()) return false;
    states_[key]=s; return true;
  }
  const ObjectState* Find(ObjectKind kind,uint64_t source) const {
    const auto it=states_.find({kind,source}); return it==states_.end() ? nullptr : &it->second;
  }
  void Clear() { states_.clear(); }
 private:
  std::map<std::pair<ObjectKind,uint64_t>,ObjectState> states_;
};
// Requests carry a bounded native action, never a callback/pointer or a field
// offset. A fresh partner pose and an exact unambiguous local binding are also
// required by the game adapter before this admission policy runs.
class Requests {
 public:
  bool Admit(const ObjectState& r,double now,double distance) {
    if (!ValidObjectRequest(r) || !std::isfinite(now) || now<0 ||
        !std::isfinite(distance) || distance<0 || r.number<=last_) return false;
    last_=r.number;
    if (now-window_>=1) { window_=now; count_=0; }
    if (++count_>16) return false;
    return r.kind==ObjectKind::Breakable ? distance<=80 :
        r.kind==ObjectKind::Door && r.value<8 && distance<=6;
  }
 private:
  uint32_t last_=0;
  double window_=0;
  unsigned count_=0;
};
inline uint32_t DamageDelta(uint32_t local,uint32_t authority) {
  return authority>local ? std::min(authority-local,100000u) : 0;
}
// Native time advances only while the host door runs. Heartbeat expiry and an
// explicit release reset progress; packet duplication never advances the clock.
class LeverHold {
 public:
  void Touch(bool holding,double now) {
    if (!holding || !std::isfinite(now)) { *this={}; return; }
    if (last_<0 || now<last_ || now-last_>.75) *this={};
    last_=now;
  }
  bool Step(double now,float dt,float duration) {
    if (!std::isfinite(now) || last_<0 || now<last_ || now-last_>.75) { *this={}; return false; }
    if (done_ || !std::isfinite(dt) || dt<=0 || dt>.1f ||
        !std::isfinite(duration) || duration<0 || duration>3600) return false;
    // Native 8256D088 completes immediately for an authored zero duration.
    elapsed_+=dt;
    if (elapsed_+1e-6<duration) return false;
    done_=true; return true;
  }
 private:
  double last_=-1,elapsed_=0;
  bool done_=false;
};
} // namespace kk::coop::objects
