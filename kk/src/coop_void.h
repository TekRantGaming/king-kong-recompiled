#pragma once
#include "coop_revive.h"
#include <algorithm>
#include <array>

namespace kk::coop::void_rescue {
constexpr double kSampleSeconds=.5, kMaxAge=30, kWindow=30;
constexpr unsigned kSpots=60, kLimit=3;
struct Gate {
  bool coop=false, jack=false, playing=false, cinematic=false, dead=false;
  bool grounded=false, native_void=false, immune=false, scripted=false;
  float fall=0;
};
inline bool Eligible(const Gate& g) {
  return g.coop && g.jack && g.playing && !g.cinematic && !g.dead && !g.scripted;
}
inline bool Confirmed(const Gate& g) {
  return Eligible(g) && !g.grounded && g.native_void && !g.immune &&
      std::isfinite(g.fall) && g.fall>3.f;
}
struct Spot {
  ShotVector position;
  float facing=0;
  int8_t nav=0;
  uint32_t floor=0;
  std::array<uint32_t,16> transform{};
  double time=0;
};
class History {
 public:
  void ClearSpots() { count_=0; next_=0; sampled_=-1; }
  bool Due(double now) const { return std::isfinite(now) && (sampled_<0 || now<sampled_ || now-sampled_>=kSampleSeconds); }
  void Sampled(double now) { sampled_=now; }
  void Remember(Spot spot,double now) {
    if (!std::isfinite(now) || !std::isfinite(spot.position.x) || !std::isfinite(spot.position.y) ||
        !std::isfinite(spot.position.z) || !std::isfinite(spot.facing)) return;
    spot.time=now; spots_[next_]=spot; next_=(next_+1)%kSpots; count_=std::min(count_+1,kSpots);
  }
  bool Allowed(double now) const {
    if (COOP_MUT_IF("void_rescue_limit")) return true;
    unsigned recent=0;
    for (unsigned i=0;i<used_;++i) if (now<rescues_[i] || now-rescues_[i]<kWindow) ++recent;
    return std::isfinite(now) && recent<kLimit;
  }
  template<class Check> std::optional<Spot> Find(const Gate& gate,double now,Check check) const {
    if (COOP_MUT_IF("void_rescue_off") || !Confirmed(gate) || !Allowed(now)) return {};
    for (unsigned i=0;i<count_;++i) {
      const auto& s=spots_[(next_+kSpots-1-i)%kSpots];
      if (now>=s.time && now-s.time<=kMaxAge &&
          (COOP_MUT_IF("void_rescue_recheck") || check(s))) return s;
    }
    return {};
  }
  void Rescued(double now) { rescues_[rescue_next_]=now; rescue_next_=(rescue_next_+1)%kLimit; used_=std::min(used_+1,kLimit); }
 private:
  std::array<Spot,kSpots> spots_{};
  std::array<double,kLimit> rescues_{};
  unsigned count_=0,next_=0,used_=0,rescue_next_=0;
  double sampled_=-1;
};
} // namespace kk::coop::void_rescue
