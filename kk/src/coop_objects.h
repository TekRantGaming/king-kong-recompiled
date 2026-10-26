#pragma once
#include "coop_enemies.h"
#include "coop_pose.h"
#include "coop_stimulus_debug.h"
#include <map>
#include <set>
#include <bit>

namespace kk::coop::objects {
constexpr size_t kQueue=kObjectQueue, kCensus=kObjectCensus;
inline bool StaticFireParameters(uint32_t mode,uint32_t repeat,uint32_t completion,float total,float timer) {
  return mode==0 && repeat==0 && completion==0 && std::isfinite(total) && total>=0 && total<=138.6f &&
      std::isfinite(timer) && (timer==-1 || (timer>=0 && timer<=3600));
}
struct PathTarget { uint32_t key=0,actor=0; };
// Native8240C5B8's mode0 lookup, with bounds/ambiguity checks that the native
// routine lacks. A zero target is a valid END only after the first segment.
template<class Read> std::optional<PathTarget> FirstPathTarget(Read read,uint32_t actor,uint32_t key=0) {
  if (!PoseAddress(actor,32)) return {};
  const auto ext=read(actor+24); if (!PoseAddress(ext,28)) return {};
  const auto table=read(ext+24); if (!PoseAddress(table,8)) return {};
  const auto groups=read(table),count=read(table+4);
  if (!count || count>kCensus || !PoseAddress(groups,count*12)) return {};
  if (!key) key=read(groups);
  if (!key) return {};
  uint32_t group=0; unsigned matches=0;
  for (unsigned i=0;i<count;++i) if (read(groups+i*12)==key) { group=groups+i*12; ++matches; }
  if (matches!=1) return {};
  const auto entries=read(group+4),length=read(group+8);
  if (length>kCensus || (length && !PoseAddress(entries,length*16))) return {};
  const auto target=length ? read(entries+8) : 0;
  if (target && !PoseAddress(target,32)) return {};
  return PathTarget{key,target};
}
template<class Read,class Half> std::optional<PathTarget> RaftPathTarget(Read read,Half half,uint32_t actor,uint32_t key) {
  const auto first=FirstPathTarget(read,actor,key); if (!first) return {};
  key=first->key;
  const auto ext=read(actor+24),table=read(ext+24),groups=read(table),count=read(table+4);
  if (!PoseAddress(ext,42)) return {};
  uint32_t group=0;
  for (unsigned i=0;i<count;++i) if (read(groups+12*i)==key) group=groups+12*i;
  const auto entries=read(group+4),length=read(group+8);
  if (length>60) return {}; // Native824FE598 has only60 scratch slots.
  uint32_t target=length ? read(entries+8) : 0; unsigned flagged=0;
  for (unsigned i=0;i<length;++i) if (half(entries+16*i)&1) { target=read(entries+16*i+8); ++flagged; }
  if (flagged>1 || (target && !PoseAddress(target,32))) return {}; // No random branch replay.
  if (!target && (read(actor)&64) && (half(ext+40)&2)) return {}; // Alternate-key redirect unsupported.
  return PathTarget{key,target};
}
struct BreakableTrack { uint32_t block=0,slot=0; bool init=false; };
// Only these two recovered callbacks may be woken by a retained outcome.
// The native dispatcher needs this AI's own state word and slot, not the outer AI's.
template<class Read,class ReadHalf> std::optional<BreakableTrack> BreakableConsumer(Read read,ReadHalf half,uint32_t ai) {
  if (!PoseAddress(ai,88)) return {};
  const auto first=half(ai+80),last=half(ai+82);
  if (first>last || last>=5) return {};
  std::optional<BreakableTrack> found;
  for (uint32_t slot=first;slot<=last;++slot) {
    const auto block=read(ai+(slot+6)*4);
    if (!PoseAddress(block,24)) continue;
    const auto fn=read(block+20);
    if (fn!=0x82516128 && fn!=0x82515CF0) continue;
    if (found) return {}; // No guessed choice between competing consumers.
    found=BreakableTrack{block,slot,fn==0x82516128};
  }
  return found;
}
// Native82516128 takes its terminal cleanup branch for this registered cap bit.
// Native82714E40 marks the obstacle itself with BOTH queued-removal flags only
// after the initializer has visited its parts. This is not a raw damage latch.
template<class Read> bool BreakableCheckpointGone(Read read,uint32_t actor,uint32_t vars) {
  if (!PoseAddress(actor,32) || !PoseAddress(vars,580)) return false;
  const auto cap=read(vars+564),bank=read(0x82CC92F8);
  if (cap<201 || cap>=1500 || (read(actor+4)&0x00600000)!=0x00600000 ||
      !PoseAddress(bank,(771+(cap>>5))*4)) return false;
  return (read(bank+(770+(cap>>5))*4)&(1u<<(cap&31)))!=0;
}
// ODE Init826EA1A0 can finish a retained collapse through native checkpoint
// cleanup instead of advancing to Wait. Certify the root AND every authored
// part as removed; a checkpoint bit alone does not prove geometry cleanup.
template<class Read> bool OdeCheckpointGone(Read read,uint32_t actor,uint32_t vars,
                                           std::span<const uint32_t> parts) {
  if (!PoseAddress(actor,32) || !PoseAddress(vars,3376) || parts.empty() || parts.size()>32 || !read(vars+4)) return false;
  const auto cap=read(vars+8),bank=read(0x82CC92F8);
  if (cap<201 || cap>=1500 || (read(actor+4)&0x00600000)!=0x00600000 ||
      !PoseAddress(bank,(771+(cap>>5))*4) || !(read(bank+(770+(cap>>5))*4)&(1u<<(cap&31)))) return false;
  for (unsigned i=0;i<parts.size();++i)
    if (!PoseAddress(parts[i],32) || read(vars+20+4*i)!=parts[i] || (read(parts[i]+4)&0x00600000)!=0x00600000) return false;
  return !read(vars+20+4*parts.size());
}
// 826F9F30 replaces the running track, not just its callback's side effects.
// Only authorize the observed ODE Active -> loaded native End program.
template<class Read,class ReadHalf> uint32_t OdeEndProgram(Read read,ReadHalf half,uint32_t ai,uint32_t actor) {
  if (!PoseAddress(ai,88) || !PoseAddress(actor,32) || read(0x82CCBF64)!=ai || read(0x82CCBF6C)!=actor) return 0;
  const auto slot=read(0x82CCBF58),first=half(ai+80),last=half(ai+82);
  if (first>last || last>=5 || slot<first || slot>last) return 0;
  const auto active=read(ai+(slot+6)*4),end=read(0x82CC8538);
  if (!PoseAddress(active,24) || read(0x82CCBF5C)!=active || read(active+20)!=0x826F1E08 ||
      !PoseAddress(end,24) || read(end+20)!=0x826F4948) return 0;
  return end;
}
// Native stop 826F9DC0 supports five slots. Never use an outer actor's current
// index, an unrelated track or a malformed scheduler range for a side call.
template<class Read,class ReadHalf> std::vector<unsigned> DoorSlots(Read read,ReadHalf half,uint32_t ai) {
  if (!PoseAddress(ai,88)) return {};
  const auto first=half(ai+80),last=half(ai+82);
  if (first>last || last>=5) return {};
  std::vector<unsigned> out;
  for (unsigned slot=first;slot<=last;++slot) {
    const auto block=read(ai+(slot+6)*4);
    if (!PoseAddress(block,24)) continue;
    const auto fn=read(block+20);
    if (fn==0x8256C3F8 || fn==0x82543478 || fn==0x82574D80) out.push_back(slot);
  }
  return out;
}
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
  if (!key || uint8_t(kind)<1 || uint8_t(kind)>6) return 0;
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
    return (r.kind==ObjectKind::Breakable || r.kind==ObjectKind::Ode || r.kind==ObjectKind::Tree) ? distance<=80 :
        (r.kind==ObjectKind::Door || r.kind==ObjectKind::Fire) && distance<=6;
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
inline bool NearLever(float dx,float dy,float dz,float fx,float fy) {
  if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz) ||
      !std::isfinite(fx) || !std::isfinite(fy) || std::abs(dz)>1.5f ||
      std::hypot(dx,dy,dz)>2) return false;
  const auto distance=std::hypot(dx,dy),length=std::hypot(fx,fy);
  return length>.1f && (distance<.1f || (dx*fx+dy*fy)>=.5f*distance*length);
}
class LeverHold {
 public:
  bool Active(double now) const {
    return std::isfinite(now) && last_>=0 && now>=last_ && now-last_<=.75;
  }
  void Touch(bool holding,double now) {
    if (!holding || !std::isfinite(now)) { *this={}; return; }
    if (last_<0 || now<last_ || now-last_>.75) *this={};
    last_=now;
  }
  bool Step(double now,float dt,float duration) {
    if (!Active(now)) { *this={}; return false; }
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
