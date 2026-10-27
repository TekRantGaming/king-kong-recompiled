#pragma once
#include "coop_protocol.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <optional>

namespace kk::coop {
// Pure camera math/buffering, shared by transport, native integration and tests.
using CameraAxes = std::array<std::array<float,3>,3>;
// Freeze through chapter-start/render hitches; receipt time, never host clock,
// owns the lease. A repeated source timestamp cannot renew it.
inline constexpr double kCameraExpirySeconds=8.0;
// A Together cinematic owns a separate lease from the downed-player request.
// Skip edges survive the end-camera packet long enough to reach native input.
class TogetherCutscene {
 public:
  void Update(const std::optional<CameraPose>& p,uint32_t generation,uint32_t level,double now) {
    if (!p || p->generation!=generation || p->level!=level || !std::isfinite(now)) { *this={}; return; }
    if (generation_!=generation) { *this={}; generation_=generation; skip_=p->skip; }
    else if (p->skip>skip_) { skip_=p->skip; skip_until_=now+.25; }
    active_=p->scripted;
  }
  bool Active() const { return active_; }
  bool Skip(double now) const { return skip_until_>0 && now<skip_until_ && now>=skip_until_-.25; }
 private:
  uint32_t generation_=0, skip_=0;
  double skip_until_=0;
  bool active_=false;
};
struct CameraDiagnostics {
  double received=-1, accepted=-1, rejected_at=-1;
  uint32_t sequence=0, accepted_sequence=0;
  uint64_t accepted_count=0, rejected_count=0;
  const char* rejection="none";
};
inline float CameraDot(const CameraPose& a,const CameraPose& b) {
  return a.qx*b.qx+a.qy*b.qy+a.qz*b.qz+a.qw*b.qw;
}
inline CameraAxes CameraBasis(const CameraPose& p) {
  const float inv=1/std::sqrt(CameraDot(p,p));
  const float x=p.qx*inv,y=p.qy*inv,z=p.qz*inv,w=p.qw*inv;
  return {{{1-2*(y*y+z*z),2*(x*y+z*w),2*(x*z-y*w)},
           {2*(x*y-z*w),1-2*(x*x+z*z),2*(y*z+x*w)},
           {2*(x*z+y*w),2*(y*z-x*w),1-2*(x*x+y*y)}}};
}
inline bool CameraRotation(CameraPose& p,const CameraAxes& axes) {
  auto dot=[](const auto& a,const auto& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; };
  for (unsigned i=0;i<3;++i) {
    if (!std::isfinite(dot(axes[i],axes[i])) || std::abs(dot(axes[i],axes[i])-1)>.01f) return false;
    for (unsigned j=0;j<i;++j) if (std::abs(dot(axes[i],axes[j]))>.01f) return false;
  }
  const auto& r=axes[0]; const auto& f=axes[1]; const auto& u=axes[2];
  const float determinant=r[0]*(f[1]*u[2]-f[2]*u[1])-f[0]*(r[1]*u[2]-r[2]*u[1])+u[0]*(r[1]*f[2]-r[2]*f[1]);
  if (determinant<.99f) return false;
  // axes are columns of the rotation matrix.
  const float trace=r[0]+f[1]+u[2];
  if (trace>0) {
    const float s=2*std::sqrt(trace+1); p.qw=s/4;
    p.qx=(f[2]-u[1])/s; p.qy=(u[0]-r[2])/s; p.qz=(r[1]-f[0])/s;
  } else if (r[0]>f[1] && r[0]>u[2]) {
    const float s=2*std::sqrt(1+r[0]-f[1]-u[2]); p.qx=s/4;
    p.qw=(f[2]-u[1])/s; p.qy=(f[0]+r[1])/s; p.qz=(u[0]+r[2])/s;
  } else if (f[1]>u[2]) {
    const float s=2*std::sqrt(1+f[1]-r[0]-u[2]); p.qy=s/4;
    p.qw=(u[0]-r[2])/s; p.qx=(f[0]+r[1])/s; p.qz=(u[1]+f[2])/s;
  } else {
    const float s=2*std::sqrt(1+u[2]-r[0]-f[1]); p.qz=s/4;
    p.qw=(r[1]-f[0])/s; p.qx=(u[0]+r[2])/s; p.qy=(u[1]+f[2])/s;
  }
  const float n=std::sqrt(CameraDot(p,p));
  if (!std::isfinite(n) || n<.5f) return false;
  p.qx/=n; p.qy/=n; p.qz/=n; p.qw/=n; return true;
}
// Observer validation compares a delayed host stream with the recent native
// scene path, rather than requiring a moving camera to occupy the same spot
// on both PCs at this instant. Remote clocks never index the local history.
class ObserverSceneHistory {
 public:
  void Clear() { poses_.clear(); }
  void Push(CameraPose p) {
    if (!ValidCameraPose(p)) { Clear(); return; }
    if (!poses_.empty() && (p.generation!=poses_.back().generation || p.level!=poses_.back().level || p.time<poses_.back().time)) Clear();
    poses_.push_back(p);
    while (poses_.size()>256 || p.time-poses_.front().time>1) poses_.pop_front();
  }
  bool Matches(const CameraPose& remote) const {
    if (!ValidCameraPose(remote)) return false;
    return std::any_of(poses_.begin(),poses_.end(),[&](const CameraPose& local) {
      if (local.generation!=remote.generation || local.level!=remote.level || local.scripted!=remote.scripted) return false;
      if (!local.scripted) return true;
      return std::hypot(local.x-remote.x,local.y-remote.y,local.z-remote.z)<=5 && std::abs(CameraDot(local,remote))>=.9f;
    });
  }
 private:
  std::deque<CameraPose> poses_;
};
inline CameraPose BlendCamera(const CameraPose& a,const CameraPose& b,float t) {
  if (a.generation!=b.generation || a.level!=b.level || a.cut!=b.cut || a.scripted!=b.scripted) return b;
  t=std::clamp(t,0.f,1.f);
  CameraPose p=b; p.time=a.time+(b.time-a.time)*t;
  p.x=a.x+(b.x-a.x)*t; p.y=a.y+(b.y-a.y)*t; p.z=a.z+(b.z-a.z)*t;
  p.fov=a.fov+(b.fov-a.fov)*t;
  float d=CameraDot(a,b), sign=d<0 ? -1.f : 1.f; d=std::clamp(std::abs(d),0.f,1.f);
  float wa=1-t,wb=t;
  if (d<.9995f) { const float theta=std::acos(d),s=std::sin(theta); wa=std::sin((1-t)*theta)/s; wb=std::sin(t*theta)/s; }
  p.qx=wa*a.qx+wb*sign*b.qx; p.qy=wa*a.qy+wb*sign*b.qy;
  p.qz=wa*a.qz+wb*sign*b.qz; p.qw=wa*a.qw+wb*sign*b.qw;
  const float n=std::sqrt(CameraDot(p,p)); p.qx/=n; p.qy/=n; p.qz/=n; p.qw/=n;
  return p;
}
class CameraBuffer {
 public:
  bool Push(CameraPose p,double received) {
    auto reject=[&](const char* why) { rejection_=why; return false; };
    if (!ValidCameraPose(p) || !std::isfinite(received)) return reject("invalid pose/receipt clock");
    if (!poses_.empty()) {
      const auto& last=poses_.back().pose;
      if (received<poses_.back().received) return reject("receipt clock went backwards");
      if (p.generation<last.generation) return reject("old generation");
      if (p.generation==last.generation) {
        if (p.level!=last.level) return reject("level changed within generation");
        if (p.time<=last.time) return reject("duplicate/backwards source clock");
        if (p.cut<last.cut || p.skip<last.skip) return reject("cut/skip went backwards");
      }
      if (received-poses_.back().received>.5 ||
          p.generation!=last.generation || p.cut!=last.cut || p.scripted!=last.scripted ||
          std::hypot(p.x-last.x,p.y-last.y,p.z-last.z)>25) poses_.clear();
    }
    poses_.push_back({p,received}); while (poses_.size()>32) poses_.pop_front(); return true;
  }
  std::optional<CameraPose> Sample(double now) const {
    if (poses_.empty() || !std::isfinite(now) || now<poses_.back().received ||
        now-poses_.back().received>kCameraExpirySeconds) return {};
    const auto& last=poses_.back();
    const double target=last.pose.time+(now-last.received)-.075;
    if (target<=poses_.front().pose.time) return poses_.front().pose;
    for (size_t i=1;i<poses_.size();++i) if (target<=poses_[i].pose.time) {
      const auto& a=poses_[i-1].pose; const auto& b=poses_[i].pose;
      return BlendCamera(a,b,float((target-a.time)/(b.time-a.time)));
    }
    return last.pose; // Hold through loss, never extrapolate a camera through geometry.
  }
  void Clear() { poses_.clear(); }
  const char* Rejection() const { return rejection_; }
 private:
  const char* rejection_="none";
  struct Entry { CameraPose pose; double received; };
  std::deque<Entry> poses_;
};
} // namespace kk::coop
