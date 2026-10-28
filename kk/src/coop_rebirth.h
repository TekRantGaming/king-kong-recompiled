#pragma once

#include "coop_enemies.h"
#include "coop_mut.h"
#include <map>
#include <optional>
#include <utility>

namespace kk::coop::enemies {
// C107: a private player reload rebuilds every native spawner from its checkpoint, while the partner's world keeps
// its living spawned enemies. The reloaded world's native rebirths take over those living spawned identities in
// sequence order instead of new sequences, so the partner keeps every match. Only the exact spawner and template
// evidence (the SpawnId source) and family can adopt an identity; dead or retired enemies are never remembered, and
// each identity is adopted once.
class RebirthAdoption {
 public:
  void Clear() { pending_.clear(); }
  // One living spawned enemy of the world before the private reload.
  void Remember(const EnemyState& e) {
    if (!e.id.source || !e.id.sequence || e.id.family == EnemyFamily::None) return;
    if (!(e.life > 0) && !COOP_MUT_IF("rebirth_dead")) return;
    auto& list = pending_[{e.id.source, uint16_t(e.id.family)}];
    if (list.size() < kMaxEnemies) list.emplace(e.id.sequence, e);
  }
  // fresh: the identity this native birth would get with a new sequence.
  std::optional<EnemyState> Take(EnemyId fresh) {
    if (!fresh.source || !fresh.sequence || fresh.family == EnemyFamily::None) return {};
    auto it = pending_.find({fresh.source, uint16_t(fresh.family)});
    if (COOP_MUT_IF("rebirth_any_source"))
      for (it = pending_.begin(); it != pending_.end() && it->second.empty(); ++it) {}
    if (it == pending_.end() || it->second.empty()) return {};
    const auto first = it->second.begin();
    const auto e = first->second;
    if (!COOP_MUT_IF("rebirth_reuse")) it->second.erase(first);
    return e;
  }
  size_t Pending() const {
    size_t n = 0;
    for (const auto& [key, list] : pending_) n += list.size();
    return n;
  }

 private:
  std::map<std::pair<uint64_t, uint16_t>, std::map<uint32_t, EnemyState>> pending_;
};
}  // namespace kk::coop::enemies
