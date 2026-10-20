#pragma once

#include "coop_protocol.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>

namespace kk::coop::enemies {
// Narrow, explicit track-family allowlist. Never classify all PNJ scripts as enemies.
inline EnemyFamily Family(uint32_t fn) {
  switch (fn) {
    case 0x8254BCB0: case 0x825B9020: case 0x82572E40: return EnemyFamily::Raptor;
    case 0x825088A0: case 0x82516820: case 0x82516508: return EnemyFamily::Worm;
    case 0x825F2380: case 0x826055F0: case 0x825FE790: return EnemyFamily::Scorpion;
    case 0x82623058: case 0x8264DA70: case 0x82641928: return EnemyFamily::Crab;
    default: return EnemyFamily::None;
  }
}
inline uint32_t LifeOffset(EnemyFamily f) {
  switch (f) {
    case EnemyFamily::Raptor: return 20;
    case EnemyFamily::Worm: return 5392;
    // 82614DC0 -> 824FEBF8 consumes firearm hits here. V+5792 is a
    // separate type-2048 pool, so it cannot diagnose firearm damage.
    case EnemyFamily::Scorpion: return 5444;
    // CR_TRACK_Init copies V+756 to V+760; CR_exec_CHECK_Paf subtracts
    // native type-4 firearm damage at 8263FFB4 and tests zero for death.
    case EnemyFamily::Crab: return 760;
    default: return 0;
  }
}
inline uint32_t SecondaryLifeOffset(EnemyFamily f) { return f == EnemyFamily::Scorpion ? 5792 : 0; }
inline uint32_t LifeBytes(EnemyFamily f) { return std::max(LifeOffset(f), SecondaryLifeOffset(f))+4; }
// FNV-1a over explicit numeric values, independent of host endianness and heap layout.
inline uint64_t Mix(uint64_t h, uint64_t v) {
  for (unsigned i = 0; i < 8; ++i) { h ^= uint8_t(v >> (i*8)); h *= 1099511628211ull; }
  return h;
}
inline uint64_t AuthoredKey(uint32_t family, float x, float y, float z) {
  uint64_t h = Mix(14695981039346656037ull, family);
  for (float v : {x, y, z}) {
    if (!std::isfinite(v) || std::abs(v) >= 1000000) return 0;
    h = Mix(h, uint64_t(int64_t(std::llround(double(v)*64))));
  }
  return h ? h : 1;
}
inline EnemyId SpawnId(uint64_t spawner, uint64_t source, uint32_t sequence, EnemyFamily family) {
  if (!spawner || !source || !sequence || family == EnemyFamily::None) return {};
  const uint64_t h = Mix(Mix(14695981039346656037ull, spawner), source);
  return {h ? h : 1, sequence, family};
}
// Resolve the live template at creation time, after native initialization.
// Only its authored numeric slot contributes to identity, never its pointer.
inline uint64_t SpawnerTemplateKey(uint64_t spawner, uint32_t source, uint32_t primary, uint32_t secondary) {
  if (!spawner || !source) return 0;
  if (source==primary) return Mix(spawner,1592);
  if (source==secondary) return Mix(spawner,1776);
  return 0;
}
struct Binding {
  EnemyId id;
  uint32_t actor = 0, ai = 0, vars = 0, matrix = 0;
};
// Refuse ambiguity, including aliased mutable identity.
inline const Binding* Match(const EnemyId& id, std::span<const Binding> bindings) {
  const Binding* found = nullptr;
  for (const auto& b : bindings) if (b.id == id) {
    if (found || !b.actor || !b.ai || !b.vars || !b.matrix) return nullptr;
    found = &b;
  }
  if (found) for (const auto& b : bindings) if (&b != found &&
      (b.actor == found->actor || b.ai == found->ai || b.vars == found->vars || b.matrix == found->matrix)) return nullptr;
  return found;
}
struct MatchCandidate {
  Binding binding;
  float x = 0, y = 0, z = 0; // Immutable authored spawn point.
  bool available = true;
};
struct MatchRetry {
  double first = -1;
  bool ToleranceAllowed(double now) {
    if (first < 0) first = now;
    return now >= first && now-first <= 10;
  }
};
// Exact matches remain retryable indefinitely (including dormant/late activation).
// Tolerance is only for authored IDs, and requires ONE candidate in the entire
// radius, never just the nearest. Include reserved/ambiguous candidates to refuse ties.
inline const Binding* MatchAuthored(const EnemyState& host, std::span<const MatchCandidate> candidates, bool tolerance) {
  const MatchCandidate* exact = nullptr;
  unsigned exact_count = 0;
  for (const auto& c : candidates) if (c.binding.id == host.id) { exact = &c; ++exact_count; }
  const MatchCandidate* found = exact;
  if (exact_count > 1) return nullptr;
  if (!exact_count) {
    if (!tolerance || host.id.sequence) return nullptr;
    unsigned count = 0;
    for (const auto& c : candidates) if (!c.binding.id.sequence && c.binding.id.family == host.id.family &&
        std::hypot(c.x-host.spawn_x, c.y-host.spawn_y, c.z-host.spawn_z) <= .25f) { found = &c; ++count; }
    if (count != 1) return nullptr;
  }
  if (!found || !found->available) return nullptr;
  const auto& b = found->binding;
  if (!b.actor || !b.ai || !b.vars || !b.matrix) return nullptr;
  for (const auto& c : candidates) if (&c != found &&
      (c.binding.actor == b.actor || c.binding.ai == b.ai || c.binding.vars == b.vars || c.binding.matrix == b.matrix)) return nullptr;
  return &found->binding;
}
struct Record {
  EnemyState enemy;
  EnemyEventKind kind = EnemyEventKind::Spawned; // Spawned means present, possibly alive.
  EnemyState latest; // Uninterpolated host sample, diagnostics only.
};
struct Frame {
  EnemyWorld world;
  double time = 0;
  std::vector<Record> records;  // Includes terminal tombstones until load; bounded by kMaxEnemies.
};
struct Counters { uint64_t sent = 0; uint32_t matched = 0, unmatched = 0, driven = 0, owned_me = 0, owned_partner = 0; };

// Worker-owned, SDK/game-free replication policy. Reliable events retry on backpressure.
class Channel {
 public:
  void Clear() { *this = {}; }
  void Publish(const Frame& frame) {
    if (!ValidEnemyWorld(frame.world) || !std::isfinite(frame.time) || frame.time < 0 || frame.time >= 1e12 ||
        frame.records.size() > kMaxEnemies) return;
    if (frame.world != world_) { Clear(); world_ = frame.world; }
    time_ = frame.time;
    // A former owner must stop publishing, including already queued live/death
    // events. Handover's reliable final state replaces its old local history.
    auto retained = [&](const EnemyId& id, uint32_t generation) {
      return std::any_of(frame.records.begin(), frame.records.end(), [&](const auto& r) {
        return r.enemy.id == id && r.enemy.generation == generation;
      });
    };
    std::erase_if(local_, [&](const auto& entry) { return !retained(entry.first, entry.second.enemy.generation); });
    std::erase_if(pending_, [&](const auto& e) { return !retained(e.enemy.id, e.enemy.generation); });
    for (const auto& r : frame.records) {
      if (!ValidEnemy(r.enemy)) continue;
      auto it = local_.find(r.enemy.id);
      if (it != local_.end() && (it->second.kind == EnemyEventKind::Removed || it->second.enemy.life <= 0)) {
        if (it->second.kind != EnemyEventKind::Removed && r.kind == EnemyEventKind::Removed) {
          Queue(r.enemy, EnemyEventKind::Removed); it->second.kind = EnemyEventKind::Removed;
        }
        continue;
      }
      if (it == local_.end()) {
        if (local_.size() >= kMaxEnemies) continue;
        Queue(r.enemy, EnemyEventKind::Spawned);
        if (r.enemy.life <= 0) Queue(r.enemy, EnemyEventKind::Died);
        if (r.kind == EnemyEventKind::Removed) Queue(r.enemy, EnemyEventKind::Removed);
      } else {
        if (it->second.enemy.life > 0 && r.enemy.life <= 0) Queue(r.enemy, EnemyEventKind::Died);
        if (it->second.kind != EnemyEventKind::Removed && r.kind == EnemyEventKind::Removed)
          Queue(r.enemy, EnemyEventKind::Removed);
      }
      local_[r.enemy.id] = r;
    }
  }
  const EnemyEvent* Event() const { return pending_.empty() ? nullptr : &pending_.front(); }
  void SentEvent() { pending_.pop_front(); }
  std::vector<EnemySnapshot> Snapshots() const {
    std::vector<EnemySnapshot> out;
    if (!ValidEnemyWorld(world_)) return out;
    out.push_back({world_, time_, {}});
    for (const auto& [id, r] : local_) {
      if (r.kind == EnemyEventKind::Removed) continue;
      if (out.back().enemies.size() == kMaxEnemyBatch) out.push_back({world_, time_, {}});
      out.back().enemies.push_back(r.enemy);
    }
    return out;
  }
  void Receive(const Message& m, double now) {
    const bool event = m.type == MessageType::EnemyEvent;
    const auto w = event ? m.enemy_event.world : m.enemies.world;
    if (!ValidEnemyWorld(w)) return;
    if (w != world_) {
      // Same checkpoint visit and global packet order both guard against old loads.
      if (world_.serial && ((w.level == world_.level && w.start == world_.start && w.serial < world_.serial) ||
          int32_t(m.sequence-world_sequence_) <= 0)) return;
      Clear(); world_ = w;
    }
    if (!world_sequence_ || int32_t(m.sequence-world_sequence_) > 0) world_sequence_ = m.sequence;
    auto push = [&](const EnemyState& e, EnemyEventKind kind, double time) {
      if (!ValidEnemy(e)) return;
      auto it = remote_.find(e.id);
      if (it == remote_.end()) {
        if (remote_.size() >= kMaxEnemies) return;
        it = remote_.emplace(e.id, Remote{}).first;
      }
      auto& r = it->second;
      if (r.record.enemy.id.source && e.generation < r.record.enemy.generation) return;
      if (r.record.enemy.id.source && e.generation > r.record.enemy.generation) r = {};
      // Reliable death/removal wins over any late unreliable live snapshot or spawn event.
      if (event) {
        if (kind == EnemyEventKind::Spawned && r.terminal) return;
        if (kind != EnemyEventKind::Spawned || e.life <= 0) {
          r.terminal = true;
          r.record.enemy = e; r.record.enemy.life = 0;
          if (r.record.kind != EnemyEventKind::Removed) r.record.kind = kind;
          r.received = now;
          return;
        }
        if (!r.history.empty()) return; // Spawn may arrive after a newer snapshot.
        r.record = {e, kind}; r.received = now;
        return;
      }
      if (r.seen && int32_t(m.sequence - r.sequence) <= 0) return;
      r.seen = true; r.sequence = m.sequence;
      if (r.terminal) return;
      r.record = {e, e.life <= 0 ? EnemyEventKind::Died : EnemyEventKind::Spawned};
      r.received = now;
      if (e.life <= 0) { r.terminal = true; return; }
      if (!r.history.empty() && time <= r.history.back().time) return;
      r.history.push_back({time, e});
      while (r.history.size() > 8) r.history.pop_front();
    };
    if (event) push(m.enemy_event.enemy, m.enemy_event.kind, 0);
    else for (const auto& e : m.enemies.enemies) push(e, EnemyEventKind::Spawned, m.enemies.time);
  }
  Frame Sample(double now) const {
    Frame out{world_, now, {}};
    for (const auto& [id, r] : remote_) {
      // Terminal events persist until unload. Live positions expire after 500 ms.
      if (!r.terminal && (r.history.empty() || now - r.received > .5)) continue;
      Record record = r.record;
      record.latest = r.record.enemy;
      if (!r.terminal) {
        const double target = r.history.back().time + (now-r.received) - .1;
        record.enemy = r.history.front().enemy;
        for (size_t i = 1; i < r.history.size(); ++i) {
          const auto& a = r.history[i-1]; const auto& b = r.history[i];
          if (target > b.time) { record.enemy = b.enemy; continue; }
          const float t = float(std::clamp((target-a.time)/(b.time-a.time), 0., 1.));
          record.enemy = a.enemy;
          record.enemy.x += (b.enemy.x-a.enemy.x)*t; record.enemy.y += (b.enemy.y-a.enemy.y)*t;
          record.enemy.z += (b.enemy.z-a.enemy.z)*t;
          record.enemy.facing = std::remainder(a.enemy.facing + std::remainder(b.enemy.facing-a.enemy.facing, 6.2831853f)*t, 6.2831853f);
          break;
        }
        // Discrete life/action/state always use newest authoritative sample.
        record.enemy.life = r.record.enemy.life; record.enemy.action = r.record.enemy.action;
        record.enemy.secondary_life = r.record.enemy.secondary_life;
        record.enemy.item = r.record.enemy.item; record.enemy.state = r.record.enemy.state;
      }
      out.records.push_back(record);
    }
    return out;
  }
 private:
  void Queue(const EnemyState& e, EnemyEventKind kind) { pending_.push_back({world_, kind, e}); }
  struct SampleEntry { double time; EnemyState enemy; };
  struct Remote {
    Record record;
    double received = 0;
    uint32_t sequence = 0;
    bool seen = false, terminal = false;
    std::deque<SampleEntry> history;
  };
  EnemyWorld world_;
  uint32_t world_sequence_ = 0;
  double time_ = 0;
  std::map<EnemyId, Record> local_;
  std::map<EnemyId, Remote> remote_;
  std::deque<EnemyEvent> pending_;
};
} // namespace kk::coop::enemies
