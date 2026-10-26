/*
MIT License

Copyright (c) 2026 cranebeaker and contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
// Fault-model method and profile values adapted from coop-anything b877c9e.
// Copyright (c) 2026 cranebeaker and contributors. MIT notice: ../../tools/coop-chaos/LICENSE.
// KK adaptation: percentages, bounded two-copy verdict, independent outgoing streams,
// relative outage schedules, counters, and explicit validation. No socket or wall clock.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace kk::coop {
struct UdpSimulation {
  // Keep the original four-field aggregate API for existing WAN tests.
  double delay_ms = 0, jitter_ms = 0, loss_percent = 0;
  uint32_t seed = 68;
  double reorder_percent = 0, duplicate_percent = 0;
  double burst_enter_percent = 0, burst_exit_percent = 50, burst_loss_percent = 100;
  uint64_t bandwidth_bps = 0;
  double buffer_ms = 200;
  // Seconds since this transport's first Poll. Both peers may have unrelated clocks.
  double one_way_at = 0, one_way_seconds = 0;
  unsigned one_way_roles = 1; // outgoing host=1, outgoing guest=2, both=3
  double disconnect_at = 0, disconnect_seconds = 0;
};

#if defined(KK_DEV_TOOLS)
inline bool UdpSimulationEnabled(const UdpSimulation& p) {
  return p.delay_ms>0 || p.jitter_ms>0 || p.loss_percent>0 || p.reorder_percent>0 ||
      p.duplicate_percent>0 || p.burst_enter_percent>0 || p.bandwidth_bps>0 ||
      p.one_way_seconds>0 || p.disconnect_seconds>0;
}
inline bool UdpProfile(std::string_view name, UdpSimulation& out) {
  UdpSimulation p;
  if (name == "lan") p = {.5,.2,0,1001};
  else if (name == "home") p = {12,4,.5,1002};
  else if (name == "bad_wifi") {
    p = {35,25,2,1003}; p.reorder_percent=2; p.duplicate_percent=.5;
    p.burst_enter_percent=1; p.burst_exit_percent=25; p.burst_loss_percent=60;
  } else if (name == "mobile") {
    p = {70,40,1,1004}; p.reorder_percent=1;
    p.burst_enter_percent=1; p.burst_exit_percent=20; p.burst_loss_percent=50;
    p.bandwidth_bps=2000000; p.buffer_ms=300;
  } else if (name == "hell") {
    p = {200,150,8,1006}; p.reorder_percent=10; p.duplicate_percent=5;
    p.burst_enter_percent=3; p.burst_exit_percent=10; p.burst_loss_percent=90;
    p.bandwidth_bps=512000; p.buffer_ms=500;
  } else return false;
  out=p; return true;
}
inline bool ValidUdpSimulation(const UdpSimulation& p) {
  const auto range=[](double v,double max) { return std::isfinite(v) && v>=0 && v<=max; };
  return range(p.delay_ms,2000) && range(p.jitter_ms,1000) && range(p.loss_percent,100) &&
      range(p.reorder_percent,100) && range(p.duplicate_percent,100) &&
      range(p.burst_enter_percent,100) && range(p.burst_exit_percent,100) && range(p.burst_loss_percent,100) &&
      p.bandwidth_bps<=10000000000ull && range(p.buffer_ms,5000) &&
      range(p.one_way_at,86400) && range(p.one_way_seconds,3600) &&
      p.one_way_roles>=1 && p.one_way_roles<=3 && range(p.disconnect_at,86400) && range(p.disconnect_seconds,3600);
}
enum class UdpDrop { None, Loss, Burst, OneWay, Disconnect, Bandwidth };
struct UdpFaultStats {
  uint64_t seen=0, forwarded=0, dropped=0, duplicated=0, reordered=0, bytes=0, queue_full=0;
  std::array<uint64_t,6> reasons{};
};
struct UdpVerdict {
  std::array<double,2> delay{}; // seconds from admission, including serialization
  unsigned copies=0;
  UdpDrop reason=UdpDrop::None;
};
class UdpImpairment {
 public:
  void Reset(UdpSimulation p,bool host) {
    *this={}; p_=ValidUdpSimulation(p) ? p : UdpSimulation{};
    // SplitMix64, with different streams even when both roles use the same seed.
    random_=uint64_t(p_.seed) ^ (host ? 0xa0761d6478bd642full : 0xe7037ed1a0b428dbull);
    role_=host ? 1 : 2;
  }
  const UdpFaultStats& Stats() const { return stats_; }
  UdpVerdict Decide(double elapsed,size_t bytes) {
    UdpVerdict v; ++stats_.seen;
    const auto active=[&](double at,double duration) { return elapsed>=at && elapsed<at+duration; };
    if (active(p_.disconnect_at,p_.disconnect_seconds)) return Drop(UdpDrop::Disconnect);
    if ((p_.one_way_roles&role_) && active(p_.one_way_at,p_.one_way_seconds)) return Drop(UdpDrop::OneWay);
    if (bad_) { if (Chance(p_.burst_exit_percent)) bad_=false; }
    else if (Chance(p_.burst_enter_percent)) bad_=true;
    if (Chance(bad_ ? p_.burst_loss_percent : p_.loss_percent)) return Drop(bad_ ? UdpDrop::Burst : UdpDrop::Loss);
    const unsigned copies=Chance(p_.duplicate_percent) ? 2 : 1;
    for (unsigned i=0;i<copies;++i) {
      double queue=0;
      if (p_.bandwidth_bps) {
        const double begin=std::max(elapsed,free_at_);
        if ((begin-elapsed)*1000>p_.buffer_ms) {
          // A duplicate also consumes bandwidth. A tail drop of only that copy
          // does not discard an already admitted original.
          ++stats_.reasons[size_t(UdpDrop::Bandwidth)];
          if (!v.copies) return Drop(UdpDrop::Bandwidth,false);
          break;
        }
        free_at_=begin+double(bytes)*8/double(p_.bandwidth_bps); queue=free_at_-elapsed;
      }
      const bool reorder=Chance(p_.reorder_percent);
      const double latency=reorder ? 0 : std::max(0.,p_.delay_ms+(Random()*2-1)*p_.jitter_ms)/1000;
      if (reorder) ++stats_.reordered;
      v.delay[v.copies++]=queue+latency;
      ++stats_.forwarded; stats_.bytes+=bytes;
    }
    if (v.copies==2) ++stats_.duplicated;
    return v;
  }
 private:
  double Random() {
    random_+=0x9e3779b97f4a7c15ull;
    auto z=random_; z=(z^(z>>30))*0xbf58476d1ce4e5b9ull;
    z=(z^(z>>27))*0x94d049bb133111ebull;
    return double((z^(z>>31))>>11)*(1./9007199254740992.);
  }
  bool Chance(double percent) { return Random()*100<percent; }
  UdpVerdict Drop(UdpDrop why,bool count_reason=true) {
    ++stats_.dropped; if (count_reason) ++stats_.reasons[size_t(why)];
    UdpVerdict v; v.reason=why; return v;
  }
  UdpSimulation p_;
  UdpFaultStats stats_;
  uint64_t random_=0;
  unsigned role_=1;
  bool bad_=false;
  double free_at_=0;
};
#endif
} // namespace kk::coop
