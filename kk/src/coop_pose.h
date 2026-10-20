#pragma once

#include "coop_protocol.h"

#include <cmath>
#include <optional>

namespace kk::coop {
struct PoseRead {
  std::optional<PlayerPose> pose;
  const char* failure = "not-published";  // Static labels only, safe to copy between threads.
};
struct PoseVelocity {
  std::optional<PlayerPose> anchor;
  float vx = 0, vy = 0;
  void Clear() { anchor.reset(); vx=vy=0; }
  void Apply(PlayerPose& p) {
    const double dt=anchor ? p.time-anchor->time : 0;
    if (!anchor || anchor->level!=p.level || dt<0 || dt>.25) {
      anchor=p; vx=vy=0;
    } else if (dt>=.05) {
      const float dx=p.x-anchor->x, dy=p.y-anchor->y;
      vx=dx/float(dt); vy=dy/float(dt);
      if (std::hypot(dx,dy)>5 || std::hypot(vx,vy)>30) vx=vy=0;
      anchor=p;
    }
    // Repeated render/input callbacks can be much closer than a physics step.
    // Hold the last 50 ms estimate instead of amplifying a single position update.
    p.vx=vx; p.vy=vy;
  }
};
inline bool PoseAddress(uint32_t a, uint32_t size = 4) {
  return a >= 0x40000000 && a <= 0xA0000000 - size && (a & 3) == 0;
}
// Only runtime supplies guest-memory readers, on the game thread. Tests use a sparse fixture.
template <class U32, class U16, class F32>
PoseRead ReadJackPose(U32 load32, U16 load16, F32 loadf, double now) {
  const uint32_t g = load32(0x82CC97C0);
  if (!PoseAddress(g, 13132)) return {{}, "globals-span"};
  // 0x828E3B48 is the load-page mode, retained after --kk_start, not a busy flag.
  // The universe request and loading-screen thread describe an actual load.
  if (load32(g + 4) != 0) return {{}, "level-request"};
  const uint32_t loading = load32(0x82D64080 + 12);
  if (load32(0x82D64080) && (loading == 1 || loading == 2)) return {{}, "loading-screen"};
  const uint32_t actor = load32(0x82CCBF7C);
  if (!PoseAddress(actor, 28)) return {{}, "actor-span"};
  if (load32(actor) & 0x00400001u) return {{}, "actor-flags"};
  const uint32_t ext = load32(actor + 24);
  if (!PoseAddress(ext, 8)) return {{}, "ext-span"};
  const uint32_t ai = load32(ext + 4);
  if (!PoseAddress(ai, 84)) return {{}, "ai-span"};
  const uint32_t first = load16(ai + 80);
  if (first > load16(ai + 82) || first > 4096) return {{}, "track-range"};
  if (!PoseAddress(ai, (first + 7) * 4)) return {{}, "track-span"};
  const uint32_t block = load32(ai + (first + 6) * 4);
  if (!PoseAddress(block, 24)) return {{}, "track-block-span"};
  if (load32(block + 20) != 0x826718B8) return {{}, "jack-track"};
  const uint32_t matrix = load32(actor + 12), vars = load32(ai + 64);
  if (!PoseAddress(matrix, 64)) return {{}, "matrix-span"};
  if (!PoseAddress(vars, 11672)) return {{}, "vars-span"};
  PlayerPose p;
  p.time = now; p.level = load32(g + 0x18);
  p.x = loadf(matrix + 48); p.y = loadf(matrix + 52); p.z = loadf(matrix + 56);
  const float fx = -loadf(matrix + 16), fy = -loadf(matrix + 20);
  if (!std::isfinite(fx) || !std::isfinite(fy) || std::hypot(fx, fy) < .1f) return {{}, "facing"};
  p.facing = std::atan2(fy, fx); p.speed = loadf(vars + 11668);
  const uint32_t weapon = load32(g + 13128);
  p.weapon = weapon <= 4 ? uint8_t(weapon) : 0;
  p.crouched = load32(vars + 2912) != 0;
  // 82808310(actor, world, 64), used by H's contact/airborne update at
  // 826A1214. Read its bounded floor-contact list without calling native code
  // or modifying the native query's global cursor flag.
  const auto world=load32(0x828E8388);
  if (PoseAddress(world,728)) {
    const auto count=load32(world+656);
    if (count<=4096 && PoseAddress(world,728+60*count)) {
      bool grounded=false;
      const bool filtered=load32(0x828D2A74)!=0;
      for (uint32_t i=0;i<count;++i) {
        const auto contact=world+728+60*i, flags=load32(contact+48);
        if (load32(contact)!=actor && load32(contact+4)!=actor) continue;
        if (flags&(0x80043000u | (filtered ? 0x80000u : 0))) continue;
        grounded|=(flags&64)!=0;
      }
      p.airborne=!grounded;
    }
  }
  const uint32_t animation_base = load32(actor + 20);
  if (PoseAddress(animation_base)) {
    const uint32_t animation = load32(animation_base);
    if (PoseAddress(animation, 164)) {
      p.action = load16(animation + 156);
      p.item = uint8_t(load32(animation + 160) >> 24);  // Big-endian first byte.
    }
  }
  Message check; check.type = MessageType::PlayerPose; check.pose = p;
  std::vector<uint8_t> bytes;
  if (!Encode(check, bytes)) return {{}, "pose-values"};
  return {p, "ok"};
}
inline float PartnerDistance(const PlayerPose& local, const PlayerPose& partner) {
  return std::hypot(partner.x - local.x, partner.y - local.y, partner.z - local.z);
}
inline bool PartnerHere(float distance) { return distance <= 1.0f; }
}  // namespace kk::coop
