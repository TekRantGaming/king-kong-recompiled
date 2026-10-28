#pragma once
#include "coop_protocol.h"
#include "coop_mut.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>

namespace kk::coop::desync {
enum class Tier { None, Quiet, Move, Reload };
inline const char* Name(Tier t) {
  switch (t) { case Tier::Quiet:return "quiet"; case Tier::Move:return "move";
    case Tier::Reload:return "guest-reload"; default:return "none"; }
}
struct State {
  WorldDigestPage header;
  std::vector<WorldScript> scripts;
  std::vector<WorldEnemy> enemies;
  std::vector<WorldProbe> probes;
};
// Reliable pages are self-contained and versioned. Incomplete, stale, duplicate,
// inconsistent or oversized sweeps never become evidence of a divergent world.
class Assembly {
 public:
  std::optional<State> Push(const WorldDigestPage& p,double now) {
    if (!ValidWorldDigest(p) || !std::isfinite(now)) return {};
    if (p.sweep<=finished_) return {};
    if (p.part==0 && p.sweep>current_.header.sweep) {
      current_={}; current_.header=p; current_.header.scripts.clear(); current_.header.enemies.clear(); current_.header.probes.clear();
      next_=0; began_=now;
    }
    const auto& h=current_.header;
    if (p.sweep!=h.sweep || p.part!=next_ || now<began_ || now-began_>8 ||
        p.parts!=h.parts || p.generation!=h.generation || p.level!=h.level || p.checkpoint!=h.checkpoint ||
        p.start!=h.start || p.captured!=h.captured || p.enemy_world!=h.enemy_world || p.completed!=h.completed || p.script_count!=h.script_count ||
        p.enemy_count!=h.enemy_count || p.probe_count!=h.probe_count || p.extensions!=h.extensions) return {};
    current_.scripts.insert(current_.scripts.end(),p.scripts.begin(),p.scripts.end());
    current_.enemies.insert(current_.enemies.end(),p.enemies.begin(),p.enemies.end());
    current_.probes.insert(current_.probes.end(),p.probes.begin(),p.probes.end());
    if (++next_!=h.parts) return {};
    finished_=h.sweep;
    if (current_.scripts.size()!=h.script_count || current_.enemies.size()!=h.enemy_count || current_.probes.size()!=h.probe_count) return {};
    std::set<std::pair<uint64_t,uint64_t>> scripts;
    std::set<EnemyId> enemies;
    for (const auto& s:current_.scripts) if (!scripts.emplace(s.source,s.script).second) return {};
    for (const auto& e:current_.enemies) if (!enemies.insert(e.id).second) return {};
    std::set<std::pair<WorldArea,uint64_t>> probes;
    for (const auto& p:current_.probes) if (!probes.emplace(p.area,p.source).second) return {};
    return current_;
  }
 private:
  State current_;
  uint32_t finished_=0;
  uint16_t next_=0;
  double began_=0;
};
struct Evidence {
  bool quiet=false, lag=false, structural=false, wrong_level=false;
  uint64_t progress=0;
  uint64_t structural_anchor=0;
  uint32_t differences=0;
  bool Any() const { return quiet || lag || structural || wrong_level; }
};
inline std::array<uint64_t,2> CompareProbes(const std::vector<WorldProbe>& host,const std::vector<WorldProbe>& local) {
  std::array<uint64_t,2> differences{};
  if (COOP_MUT_IF("digest_probe_skip")) return differences;
  std::map<std::pair<WorldArea,uint64_t>,const WorldProbe*> rows;
  for (const auto& p:local) rows[{p.area,p.source}]=&p;
  for (const auto& h:host) {
    const auto it=rows.find({h.area,h.source});
    const auto area=size_t(h.area)-1;
    if (area>=differences.size()) continue;
    const auto* p=it==rows.end() ? nullptr:it->second;
    const float tolerance=h.area==WorldArea::Npcs ? 5.f:.1f;
    if (!p || p->value!=h.value || std::hypot(p->x-h.x,p->y-h.y,p->z-h.z)>tolerance)
      ++differences[area];
    if (it!=rows.end()) rows.erase(it);
  }
  for (const auto& [key,p]:rows) if (size_t(key.first)>=1 && size_t(key.first)<=2) ++differences[size_t(key.first)-1];
  return differences;
}
// C108: a host owned enemy the guest still holds alive is structural drift only
// while the guest has not applied that owner's retirement. After the guest's
// native destroy of a retired centipede its actor stayed bound with its last
// life (201/3); the host digest drops retired records, so the guest drops its own.
inline bool GuestOnlyAliveIsDrift(bool host_listed,bool locally_retired,bool alive) {
  if (host_listed || !alive) return false;
  return !locally_retired || COOP_MUT_IF("digest_retired");
}
// Both stamps originate on the host. A whole sweep delayed behind reliable
// traffic cannot become fresh just because its final page arrived recently.
inline bool FreshForPose(const WorldDigestPage& p,const std::optional<PlayerPose>& host) {
  return host && host->level==p.level && std::isfinite(host->time) &&
      host->time>=p.captured-1 && host->time-p.captured<=5;
}
// Arrival-clock hysteresis, never peer-clock arithmetic. Packet silence is not
// drift. Native progress and gaps restart grace. One move and one reload per
// host checkpoint, including a reload at the initial/same checkpoint.
class Guard {
 public:
  Tier Observe(uint32_t generation,uint32_t level,uint32_t checkpoint,const Evidence& e,double now) {
    if (!std::isfinite(now)) return Tier::None;
    if (COOP_MUT_IF("false_quiet")) return Tier::Quiet;
    if (generation_!=generation || level_!=level) {
      *this={}; generation_=generation; level_=level; checkpoint_=checkpoint;
    }
    if (checkpoint_!=checkpoint) {
      checkpoint_=checkpoint; seen_=false; samples_=0; last_=0; cooldown_=0; next_quiet_=0; quiet_samples_=0;
    }
    // A health or metadata repair is independent of unrelated script progress.
    // Keep the same repeated fresh sample rule, including pause/gap resets.
    if (!e.quiet || now<last_ || now-last_>8) quiet_samples_=0;
    if (e.quiet && !quiet_samples_++) quiet_since_=now;
    // Unrelated script progress or another birth cannot repair a specific
    // missing host actor. Retain grace while that same structural anchor stays
    // missing; convergence, a new anchor, stale samples and world changes reset it.
    const bool anchored=e.structural && e.structural_anchor && !e.wrong_level;
    const auto progress=anchored ? e.structural_anchor:e.progress;
    if (!e.Any() || now<last_ || now-last_>8 || (seen_ && (anchored_!=anchored || progress_!=progress))) {
      since_=now; samples_=0;
    }
    last_=now; progress_=progress; anchored_=anchored; seen_=true;
    if (!e.Any() || now<cooldown_) return Tier::None;
    if (!samples_++) since_=now;
    if (e.quiet && !e.wrong_level && quiet_samples_>=3 && now-quiet_since_>=6 && now>=next_quiet_ &&
        !COOP_MUT_IF("digest_quiet_progress")) {
      next_quiet_=now+6; return Tier::Quiet;
    }
    if (samples_<3) return Tier::None;
    if (e.wrong_level) {
      if (now-since_>20 && !reloaded_.contains(checkpoint_)) return Tier::Reload;
      return Tier::None;
    }
    if ((e.lag || e.structural) && now-since_>20 && !reloaded_.contains(checkpoint_)) {
      if (!moved_.contains(checkpoint_)) return Tier::Move;
      if (!reloaded_.contains(checkpoint_)) return Tier::Reload;
    }
    if (e.quiet && now-since_>=6 && now>=next_quiet_) {
      next_quiet_=now+6; return Tier::Quiet;
    }
    return Tier::None;
  }
  void Acted(Tier tier,double now) {
    if (tier==Tier::Move) moved_.insert(checkpoint_);
    if (tier==Tier::Reload) reloaded_.insert(checkpoint_);
    if (tier==Tier::Move || tier==Tier::Reload) {
      since_=last_=now; samples_=0; cooldown_=now+(tier==Tier::Reload ? 30 : 3);
    }
  }
  void Suspend() { seen_=false; samples_=quiet_samples_=0; last_=0; }
 private:
  uint32_t generation_=0,level_=0,checkpoint_=0;
  uint64_t progress_=0;
  bool seen_=false,anchored_=false;
  std::set<uint32_t> moved_,reloaded_;
  unsigned samples_=0;
  unsigned quiet_samples_=0;
  double quiet_since_=0;
  double since_=0,last_=0,cooldown_=0,next_quiet_=0;
};
} // namespace kk::coop::desync
