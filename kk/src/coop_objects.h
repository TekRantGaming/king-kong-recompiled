#pragma once
#include "coop_enemies.h"
#include <map>

namespace kk::coop::objects {
constexpr size_t kQueue=kObjectQueue, kCensus=kObjectCensus;
struct Binding {
  uint32_t actor=0,ai=0,vars=0,matrix=0;
  uint64_t source=0;
  ObjectKind kind=ObjectKind::Breakable;
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
    return r.kind==ObjectKind::Breakable ? distance<=80 && (r.value==1 || r.value==100) :
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
} // namespace kk::coop::objects
