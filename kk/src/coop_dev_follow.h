#pragma once
#if defined(KK_DEV_TOOLS)
#include "coop_pose.h"
#include <algorithm>

namespace kk::coop {
// Developer fixture only. Missing/frozen session evidence breaks continuity.
struct DevFollowResult { int16_t x=0, y=0; float distance=0; bool move=false; };
class DevFollowHost {
 public:
  DevFollowResult Tick(double now, const std::optional<PlayerPose>& local,
                       const std::optional<PlayerPose>& host) {
    if (!std::isfinite(now) || !local || !host || now-local->time>.5 || now<local->time) {
      since_.reset(); return {};
    }
    DevFollowResult out;
    out.distance=PartnerDistance(*local,*host);
    if (!std::isfinite(out.distance) || !std::isfinite(local->facing)) { since_.reset(); return {}; }
    const bool same=local->level==host->level;
    if (!same || out.distance>30) {
      if (!since_ || now<*since_) since_=now;
      if (now-*since_>10) { out.move=true; since_=now; }
    } else since_.reset();
    const float dx=host->x-local->x, dy=host->y-local->y, planar=std::hypot(dx,dy);
    if (same && planar>3) {
      const float fx=std::cos(local->facing), fy=std::sin(local->facing);
      out.x=int16_t(std::clamp((dx*fy-dy*fx)/planar,-1.f,1.f)*28000);
      out.y=int16_t(std::clamp((dx*fx+dy*fy)/planar,-1.f,1.f)*28000);
    }
    return out;
  }
 private:
  std::optional<double> since_;
};
}
#endif
