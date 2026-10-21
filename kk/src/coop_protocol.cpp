#include "coop_protocol.h"
#include "coop_combat.h"

#include <bit>
#include <algorithm>
#include <cmath>
#include <utility>

namespace kk::coop {
namespace {
struct Writer {
  std::vector<uint8_t> bytes;
  void U(uint64_t v, size_t n) { for (size_t i = n; i; --i) bytes.push_back(uint8_t(v >> ((i - 1) * 8))); }
  bool Text(const std::string& s) {
    if (s.empty() || s.size() > 128) return false;
    for (unsigned char c : s) if (c < 32 || c > 126) return false;
    U(s.size(), 2);
    bytes.insert(bytes.end(), s.begin(), s.end());
    return true;
  }
};
struct Reader {
  std::span<const uint8_t> bytes;
  size_t at = 0;
  bool ok = true;
  uint64_t U(size_t n) {
    if (n > bytes.size() - at) { ok = false; return 0; }
    uint64_t v = 0;
    while (n--) v = (v << 8) | bytes[at++];
    return v;
  }
  bool Text(std::string& s) {
    const size_t n = size_t(U(2));
    if (!ok || !n || n > 128 || n > bytes.size() - at) return false;
    s.assign(reinterpret_cast<const char*>(bytes.data() + at), n);
    at += n;
    for (unsigned char c : s) if (c < 32 || c > 126) return false;
    return true;
  }
};
bool ValidPose(const PlayerPose& p) {
  return std::isfinite(p.time) && p.time >= 0 && p.time < 1e12 && std::isfinite(p.x) && std::isfinite(p.y) &&
      std::isfinite(p.z) && std::isfinite(p.facing) && std::isfinite(p.speed) &&
      std::abs(p.x) < 1000000 && std::abs(p.y) < 1000000 && std::abs(p.z) < 1000000 &&
      std::abs(p.speed) < 10000 && std::abs(p.facing) <= 3.1416f &&
      std::isfinite(p.vx) && std::isfinite(p.vy) && std::isfinite(p.pitch) &&
      std::abs(p.vx) < 1000 && std::abs(p.vy) < 1000 && std::abs(p.pitch) <= 1.571f &&
      p.weapon <= 4 && p.crouched <= 1 && p.airborne <= 1 && p.fire_weapon <= 4 && (!p.fire || p.fire_weapon);
}
bool ReasonValid(RejectReason r) { return r >= RejectReason::Protocol && r <= RejectReason::Handshake; }
void WriteWorld(Writer& w, const EnemyWorld& v) { w.U(v.level, 4); w.U(v.start, 4); w.U(v.serial, 4); }
EnemyWorld ReadWorld(Reader& r) { return {uint32_t(r.U(4)), uint32_t(r.U(4)), uint32_t(r.U(4))}; }
void WriteEnemy(Writer& w, const EnemyState& e) {
  w.U(e.id.source, 8); w.U(e.id.sequence, 4); w.U(uint16_t(e.id.family), 2);
  for (float v : {e.x, e.y, e.z, e.facing, e.life}) w.U(std::bit_cast<uint32_t>(v), 4);
  w.U(e.state, 4); w.U(e.action, 2); w.U(e.item, 1);
  for (float v : {e.spawn_x, e.spawn_y, e.spawn_z}) w.U(std::bit_cast<uint32_t>(v), 4);
  w.U(uint8_t(e.owner), 1); w.U(e.generation, 4);
  w.U(std::bit_cast<uint32_t>(e.secondary_life), 4);
}
EnemyState ReadEnemy(Reader& r) {
  EnemyState e;
  e.id = {r.U(8), uint32_t(r.U(4)), EnemyFamily(r.U(2))};
  e.x = std::bit_cast<float>(uint32_t(r.U(4))); e.y = std::bit_cast<float>(uint32_t(r.U(4)));
  e.z = std::bit_cast<float>(uint32_t(r.U(4))); e.facing = std::bit_cast<float>(uint32_t(r.U(4)));
  e.life = std::bit_cast<float>(uint32_t(r.U(4)));
  e.state = uint32_t(r.U(4)); e.action = uint16_t(r.U(2)); e.item = uint8_t(r.U(1));
  e.spawn_x = std::bit_cast<float>(uint32_t(r.U(4)));
  e.spawn_y = std::bit_cast<float>(uint32_t(r.U(4)));
  e.spawn_z = std::bit_cast<float>(uint32_t(r.U(4)));
  e.owner = EnemyOwner(r.U(1)); e.generation = uint32_t(r.U(4));
  e.secondary_life = std::bit_cast<float>(uint32_t(r.U(4)));
  return e;
}
bool Point(ShotVector p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
      std::abs(p.x) < 1000000 && std::abs(p.y) < 1000000 && std::abs(p.z) < 1000000;
}
void WriteVector(Writer& w, ShotVector p) {
  for (float v : {p.x, p.y, p.z}) w.U(std::bit_cast<uint32_t>(v), 4);
}
ShotVector ReadVector(Reader& r) {
  return {std::bit_cast<float>(uint32_t(r.U(4))), std::bit_cast<float>(uint32_t(r.U(4))),
      std::bit_cast<float>(uint32_t(r.U(4)))};
}
}  // namespace
std::string SanitizeDisplayName(std::string_view name) {
  std::string out;
  bool space = false;
  // Bound work as well as output, even for a local persona made entirely of controls.
  name = name.substr(0, 512);
  for (size_t i = 0; i < name.size();) {
    const size_t begin = i;
    const auto first = uint8_t(name[i++]);
    uint32_t cp = first;
    size_t extra = 0;
    if (first >= 0xC2 && first <= 0xDF) { cp &= 0x1F; extra = 1; }
    else if (first >= 0xE0 && first <= 0xEF) { cp &= 0x0F; extra = 2; }
    else if (first >= 0xF0 && first <= 0xF4) { cp &= 7; extra = 3; }
    else if (first >= 0x80) continue;
    bool valid = i + extra <= name.size();
    for (size_t j = 0; valid && j < extra; ++j) {
      const auto c = uint8_t(name[i+j]);
      if ((c & 0xC0) != 0x80) valid = false;
      else cp = (cp << 6) | (c & 0x3F);
    }
    if (!valid) continue;
    i += extra;
    if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
        (extra == 3 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) continue;
    if (cp == 32 || cp == 9 || cp == 10 || cp == 13 || cp == 0xA0 || cp == 0x3000) {
      space = !out.empty(); continue;
    }
    if (cp < 32 || (cp >= 0x7F && cp <= 0x9F) || cp == 0xAD || cp == 0x61C ||
        cp == 0x180E || (cp >= 0x2000 && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202F) ||
        (cp >= 0x2060 && cp <= 0x206F) || cp == 0xFEFF || (cp & 0xFFFF) >= 0xFFFE ||
        (cp >= 0xFFF9 && cp <= 0xFFFB) || (cp >= 0xE0000 && cp <= 0xE007F)) continue;
    if (out.size() + size_t(space) + i - begin > kMaxDisplayNameBytes) break;
    if (space) out += ' ';
    space = false;
    out.append(name.substr(begin, i-begin));
  }
  return out.empty() ? "Partner" : out;
}
uint64_t SessionSaveHash(std::span<const uint8_t> bytes) {
  uint64_t hash = 14695981039346656037ull;
  for (uint8_t b : bytes) { hash ^= b; hash *= 1099511628211ull; }
  return hash;
}
bool IsFlowMessage(MessageType t) { return (t >= MessageType::SessionStart && t <= MessageType::FlowAbort) || IsLifeMessage(t); }
bool ValidObjectState(const ObjectState& s) {
  if (!s.generation || !s.level || s.level>100000 || !s.number || !s.source ||
      !std::isfinite(s.position) || !std::isfinite(s.seconds)) return false;
  if (s.kind==ObjectKind::Breakable) return s.value<=100000 && s.position>=0 && s.position<=5 &&
      std::floor(s.position)==s.position && s.seconds==0;
  return s.kind==ObjectKind::Door && s.value<=511 && s.position>=0 && s.position<=1 &&
      s.seconds>=0 && s.seconds<=3600;
}
bool ValidObjectRequest(const ObjectState& s) {
  if (!s.generation || !s.level || s.level>100000 || !s.number || !s.source ||
      !std::isfinite(s.position) || !std::isfinite(s.seconds)) return false;
  if (s.kind==ObjectKind::Breakable) return !(s.value&~kObjectHitFlags) && s.position>=0 &&
      s.position<=100000 && std::floor(s.position)==s.position && s.seconds>=0 && s.seconds<=2;
  return s.kind==ObjectKind::Door && s.value<8 && s.position==0 &&
      (s.seconds==0 || s.seconds==1 || s.seconds==2);
}
bool ValidStoryEvent(const StoryEvent& e) {
  if (e.phase>=StoryPhase::SnapshotRequest && e.phase<=StoryPhase::SnapshotEnd) {
    if (!e.generation || !e.level || e.level>100000 || !e.number || !e.source || !e.script ||
        !e.run || e.record>kStoryRecords) return false;
    if (StorySnapshotEntry(e.phase)) return true;
    return e.run==1 && e.record==0 && e.script<=kStorySnapshotScripts+1 &&
        (e.phase!=StoryPhase::SnapshotRequest || e.script==1);
  }
  return e.generation && e.level && e.level <= 100000 && e.number && e.source && e.script &&
      e.run && e.record <= kStoryRecords && e.phase >= StoryPhase::Armed && e.phase <= StoryPhase::Checksum &&
      ((e.phase == StoryPhase::Armed || e.phase == StoryPhase::Started) ? e.record == kStoryRecords : true) &&
      (e.phase != StoryPhase::Advance || e.record < kStoryRecords);
}
bool ValidPlayerLife(const PlayerLife& p) {
  return ValidFlowEvent(p.world) && p.incarnation && p.number && p.checkpoint <= 10000;
}
bool ValidCameraPose(const CameraPose& p) {
  const double norm = double(p.qx)*p.qx + double(p.qy)*p.qy + double(p.qz)*p.qz + double(p.qw)*p.qw;
  return p.generation && p.level && p.level <= 100000 && p.cut &&
      std::isfinite(p.time) && p.time >= 0 && p.time < 1e12 &&
      Point({p.x,p.y,p.z}) && std::isfinite(norm) && std::abs(norm-1) <= .002 &&
      std::isfinite(p.fov) && p.fov > .01f && p.fov < 3.f;
}
bool ValidSessionStart(const SessionStart& s) {
  return s.chapter < 42 && s.level > 0 && s.level <= 100000 && s.start <= 10000 &&
      s.cheats <= 0x1ff && s.difficulty <= 1 &&
      (s.save_bytes == 0 || s.save_bytes == kSessionSaveBytes) &&
      (s.save_bytes != 0 || s.save_hash == SessionSaveHash({}));
}
bool ValidFlowEvent(const FlowEvent& f) {
  return f.generation > 0 && f.level > 0 && f.level <= 100000 && f.start <= 10000 && (f.completed >> 42) == 0;
}
bool ValidEnemyWorld(const EnemyWorld& w) {
  return w.level > 0 && w.level <= 100000 && w.start <= 10000 && w.serial > 0;
}
bool ValidShotReceipt(const ShotReceipt& r) {
  return ValidEnemyWorld(r.world) && r.number && r.reason < ShotReject::Count;
}
bool ValidEnemy(const EnemyState& e) {
  return e.id.source && ValidEnemyFamily(e.id.family) &&
      std::isfinite(e.x) && std::isfinite(e.y) && std::isfinite(e.z) && std::isfinite(e.facing) &&
      std::isfinite(e.life) && std::abs(e.x) < 1000000 && std::abs(e.y) < 1000000 &&
      std::abs(e.z) < 1000000 && std::abs(e.facing) <= 3.1416f && std::abs(e.life) < 1000000 &&
      Point({e.spawn_x, e.spawn_y, e.spawn_z}) && e.owner <= EnemyOwner::Guest &&
      std::isfinite(e.secondary_life) && e.secondary_life >= 0 && e.secondary_life < 1000000 &&
      (e.id.family == EnemyFamily::Scorpion || e.secondary_life == 0);
}
bool ValidOwnershipChange(const OwnershipChange& c) {
  if (!ValidEnemyWorld(c.world) || !ValidEnemy(c.enemy) || c.owner > EnemyOwner::Guest ||
      c.phase < OwnershipPhase::Matched || c.phase > OwnershipPhase::Commit) return false;
  if (c.phase == OwnershipPhase::Matched)
    return c.generation == c.enemy.generation && c.owner == c.enemy.owner;
  if (!c.generation) return false;
  if (c.phase == OwnershipPhase::Commit)
    return c.generation == c.enemy.generation && c.owner == c.enemy.owner;
  return c.enemy.generation != UINT32_MAX && c.generation == c.enemy.generation+1 && c.owner != c.enemy.owner;
}
bool ValidShotIntent(const ShotIntent& s) {
  if (!ValidEnemyWorld(s.world) || !s.number || s.weapon < 1 || s.weapon > 4 ||
      !std::isfinite(s.time) || s.time < 0 || s.time >= 1e12 || !Point(s.origin) || !Point(s.direction) ||
      s.hits.size() > (s.weapon == 3 ? kMaxShotHits : 1)) return false;
  const float length = std::hypot(s.direction.x, s.direction.y, s.direction.z);
  if (length < .99f || length > 1.01f) return false;
  for (size_t i = 0; i < s.hits.size(); ++i) {
    const auto& hit = s.hits[i];
    if (!hit.id.source || !ValidEnemyFamily(hit.id.family) || !Point(hit.point)) return false;
    for (size_t j = 0; j < i; ++j) if (s.hits[j].id == hit.id) return false;
  }
  return true;
}
bool ValidInventory(const InventoryState& i) {
  if (!ValidEnemyWorld(i.world) || !std::isfinite(i.time) || i.time < 0 || i.time >= 1e12 ||
      i.weapon > 4) return false;
  for (auto ammo : i.ammo) if (ammo > 1000000) return false;
  return true;
}

bool ValidDamageEvent(const DamageEvent& d) {
  const float length = std::hypot(d.direction.x, d.direction.y, d.direction.z);
  return ValidEnemyWorld(d.world) && d.incarnation && d.number && d.enemy.source &&
      d.enemy.family == EnemyFamily::Raptor && combat::RaptorHit(d.flags, d.magnitude) &&
      Point(d.direction) && length >= .9f && length <= 1.1f && std::isfinite(d.time) && d.time >= 0 && d.time < 1e12;
}
bool ValidPlayerStatus(const PlayerStatus& p) {
  return ValidEnemyWorld(p.world) && p.incarnation && p.number &&
      p.condition >= PlayerCondition::Alive && p.condition <= PlayerCondition::Dead;
}
bool ValidWorldDigest(const WorldDigestPage& p) {
  if (!p.generation || !p.level || p.level>100000 || p.start>10000 || p.checkpoint>10000 || !p.sweep ||
      !p.parts || p.parts>256 || p.part>=p.parts || p.script_count>kStorySnapshotScripts || p.enemy_count>kMaxEnemies ||
      p.extensions || !std::isfinite(p.captured) || p.captured<0 || p.captured>=1e12 ||
      !ValidEnemyWorld(p.enemy_world) || p.enemy_world.level!=p.level || p.enemy_world.start!=p.start ||
      p.scripts.size()>kDigestScriptsPerPage || p.enemies.size()>kDigestEnemiesPerPage ||
      p.scripts.size()>p.script_count || p.enemies.size()>p.enemy_count ||
      p.parts!=std::max<size_t>({1,(size_t(p.script_count)+15)/16,(size_t(p.enemy_count)+11)/12})) return false;
  const auto expected=[](size_t total,size_t part,size_t per) { return std::min(per,total-std::min(total,part*per)); };
  if (p.scripts.size()!=expected(p.script_count,p.part,kDigestScriptsPerPage) ||
      p.enemies.size()!=expected(p.enemy_count,p.part,kDigestEnemiesPerPage)) return false;
  for (const auto& s:p.scripts) if (!s.source || !s.script || s.record>kStoryRecords || s.flags>7 ||
      (!s.run && s.flags)) return false;
  for (const auto& e:p.enemies) if (!e.id.source || !ValidEnemyFamily(e.id.family) ||
      e.owner>EnemyOwner::Guest || !std::isfinite(e.life) || std::abs(e.life)>=1000000 ||
      !std::isfinite(e.secondary_life) || e.secondary_life<0 || e.secondary_life>=1000000 ||
      (e.id.family!=EnemyFamily::Scorpion && e.secondary_life!=0)) return false;
  return true;
}
bool Encode(const Message& m, std::vector<uint8_t>& out) {
  Writer payload;
  switch (m.type) {
    case MessageType::ObjectState: case MessageType::ObjectRequest:
      if (!(m.type==MessageType::ObjectRequest ? ValidObjectRequest(m.object) : ValidObjectState(m.object))) return false;
      payload.U(m.object.generation,4); payload.U(m.object.level,4); payload.U(m.object.number,4);
      payload.U(m.object.source,8); payload.U(uint8_t(m.object.kind),1); payload.U(m.object.value,4);
      payload.U(std::bit_cast<uint32_t>(m.object.position),4); payload.U(std::bit_cast<uint32_t>(m.object.seconds),4); break;
    case MessageType::WorldDigest: {
      const auto& p=m.digest; if (!ValidWorldDigest(p)) return false;
      for (uint32_t v:{p.generation,p.level,p.start,p.checkpoint,p.sweep}) payload.U(v,4);
      payload.U(std::bit_cast<uint64_t>(p.captured),8);
      WriteWorld(payload,p.enemy_world);
      payload.U(p.completed,8);
      for (uint16_t v:{p.part,p.parts,p.script_count,p.enemy_count}) payload.U(v,2);
      payload.U(p.extensions,4); payload.U(p.scripts.size(),1); payload.U(p.enemies.size(),1);
      for (const auto& s:p.scripts) {
        payload.U(s.source,8); payload.U(s.script,8); payload.U(s.run,4); payload.U(s.record,4); payload.U(s.flags,1);
      }
      for (const auto& e:p.enemies) {
        payload.U(e.id.source,8); payload.U(e.id.sequence,4); payload.U(uint16_t(e.id.family),2);
        payload.U(uint8_t(e.owner),1); payload.U(e.generation,4);
        payload.U(std::bit_cast<uint32_t>(e.life),4); payload.U(std::bit_cast<uint32_t>(e.secondary_life),4);
      }
      break;
    }
    case MessageType::StoryEvent: case MessageType::TriggerRequest:
      if (!ValidStoryEvent(m.story) || (m.type == MessageType::TriggerRequest && !ValidStoryRequest(m.story.phase)) ||
          (m.type == MessageType::StoryEvent && m.story.phase==StoryPhase::SnapshotRequest)) return false;
      payload.U(m.story.generation,4); payload.U(m.story.level,4); payload.U(m.story.number,4);
      payload.U(m.story.source,8); payload.U(m.story.script,8);
      payload.U(m.story.run,4); payload.U(m.story.record,4); payload.U(uint8_t(m.story.phase),1); break;
    case MessageType::OwnershipChange:
      if (!ValidOwnershipChange(m.ownership)) return false;
      WriteWorld(payload, m.ownership.world); payload.U(uint8_t(m.ownership.phase), 1);
      payload.U(m.ownership.generation, 4); payload.U(uint8_t(m.ownership.owner), 1);
      WriteEnemy(payload, m.ownership.enemy); break;
    case MessageType::PlayerDowned: case MessageType::PlayerRevived:
      if (!ValidPlayerLife(m.life)) return false;
      payload.U(m.life.world.generation, 4); payload.U(m.life.world.level, 4);
      payload.U(m.life.world.start, 4); payload.U(m.life.world.completed, 8);
      payload.U(m.life.incarnation, 8); payload.U(m.life.number, 4); payload.U(m.life.checkpoint, 4);
      break;
    case MessageType::CameraPose: case MessageType::CameraCut:
      if (!ValidCameraPose(m.camera)) return false;
      payload.U(m.camera.generation,4); payload.U(m.camera.level,4);
      payload.U(m.camera.cut,4); payload.U(m.camera.skip,4);
      payload.U(std::bit_cast<uint64_t>(m.camera.time),8);
      for (float f : {m.camera.x,m.camera.y,m.camera.z,m.camera.qx,m.camera.qy,m.camera.qz,m.camera.qw,m.camera.fov})
        payload.U(std::bit_cast<uint32_t>(f),4);
      payload.U(unsigned(m.camera.scripted) | (unsigned(m.camera.paused)<<1),1); break;
    case MessageType::DamageEvent:
      if (!ValidDamageEvent(m.damage)) return false;
      WriteWorld(payload, m.damage.world); payload.U(m.damage.incarnation, 8); payload.U(m.damage.number, 4);
      payload.U(m.damage.enemy.source, 8); payload.U(m.damage.enemy.sequence, 4); payload.U(uint16_t(m.damage.enemy.family), 2);
      payload.U(m.damage.flags, 4); payload.U(m.damage.magnitude, 4); WriteVector(payload, m.damage.direction);
      payload.U(std::bit_cast<uint64_t>(m.damage.time), 8);
      break;
    case MessageType::PlayerStatus:
      if (!ValidPlayerStatus(m.player_status)) return false;
      WriteWorld(payload, m.player_status.world); payload.U(m.player_status.incarnation, 8);
      payload.U(m.player_status.number, 4); payload.U(m.player_status.last_damage, 4);
      payload.U(uint8_t(m.player_status.condition), 1); break;
    case MessageType::Cheats:
      if (!ValidCheatSet(m.cheats)) return false;
      payload.U(m.cheats.bits, 2); break;
    case MessageType::InventoryState:
      if (!ValidInventory(m.inventory)) return false;
      WriteWorld(payload, m.inventory.world);
      payload.U(std::bit_cast<uint64_t>(m.inventory.time), 8);
      payload.U(m.inventory.weapon, 1);
      for (auto ammo : m.inventory.ammo) payload.U(ammo, 4);
      break;
    case MessageType::SessionStart:
      if (!ValidSessionStart(m.start)) return false;
      payload.U(m.start.chapter, 2); payload.U(m.start.level, 4); payload.U(m.start.start, 4);
      payload.U(m.start.chapter_select, 1); payload.U(m.start.cheats, 2); payload.U(m.start.difficulty, 1);
      payload.U(m.start.save_bytes, 4); payload.U(m.start.save_hash, 8);
      break;
    case MessageType::SaveChunk:
      if (m.save_chunk.bytes.empty() || m.save_chunk.bytes.size() > kSaveChunkBytes ||
          m.save_chunk.offset > kSessionSaveBytes || m.save_chunk.bytes.size() > kSessionSaveBytes - m.save_chunk.offset) return false;
      payload.U(m.save_chunk.offset, 4); payload.U(m.save_chunk.bytes.size(), 2);
      payload.bytes.insert(payload.bytes.end(), m.save_chunk.bytes.begin(), m.save_chunk.bytes.end());
      break;
    case MessageType::LevelChange: case MessageType::Loaded: case MessageType::LoadRelease:
    case MessageType::Checkpoint: case MessageType::ChapterFinished: case MessageType::ReloadRequest:
    case MessageType::FlowAbort:
      if (!ValidFlowEvent(m.flow)) return false;
      payload.U(m.flow.generation, 4); payload.U(m.flow.level, 4); payload.U(m.flow.start, 4); payload.U(m.flow.completed, 8);
      break;
    case MessageType::Hello:
      payload.U(m.hello.protocol, 2);
      if (!payload.Text(m.hello.version) || !payload.Text(m.hello.build) || !payload.Text(m.hello.gameplay_digest))
        return false;
      if (m.display_name.size() > kMaxDisplayNameBytes || SanitizeDisplayName(m.display_name) != m.display_name)
        return false;
      payload.U(m.display_name.size(), 1);
      payload.bytes.insert(payload.bytes.end(), m.display_name.begin(), m.display_name.end());
      if (!m.join_code.empty()) {
        if (m.join_code.size() != 6) return false;
        for (unsigned char c : m.join_code) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
        payload.U(m.join_code.size(), 1);
        payload.bytes.insert(payload.bytes.end(), m.join_code.begin(), m.join_code.end());
      }
      break;
    case MessageType::Reject:
      if (!ReasonValid(m.reason)) return false;
      payload.U(uint16_t(m.reason), 2);
      break;
    case MessageType::Ping: case MessageType::Pong: payload.U(m.clock_ms, 8); break;
    case MessageType::PlayerPose:
      if (!ValidPose(m.pose)) return false;
      payload.U(std::bit_cast<uint64_t>(m.pose.time), 8);
      payload.U(m.pose.level, 4);
      for (float v : {m.pose.x, m.pose.y, m.pose.z, m.pose.facing, m.pose.speed})
        payload.U(std::bit_cast<uint32_t>(v), 4);
      payload.U(m.pose.action, 2);
      payload.U(m.pose.item, 1);
      for (float v : {m.pose.vx, m.pose.vy, m.pose.pitch}) payload.U(std::bit_cast<uint32_t>(v), 4);
      payload.U(m.pose.weapon, 1); payload.U(m.pose.crouched | (m.pose.airborne<<1), 1);
      payload.U(m.pose.fire, 4); payload.U(m.pose.fire_weapon, 1);
      break;
    case MessageType::HelloAck: case MessageType::Bye: break;
    case MessageType::EnemySnapshot:
      if (!ValidEnemyWorld(m.enemies.world) || !std::isfinite(m.enemies.time) || m.enemies.time < 0 ||
          m.enemies.time >= 1e12 || m.enemies.enemies.size() > kMaxEnemyBatch) return false;
      WriteWorld(payload, m.enemies.world);
      payload.U(std::bit_cast<uint64_t>(m.enemies.time), 8);
      payload.U(m.enemies.enemies.size(), 2);
      for (size_t i = 0; i < m.enemies.enemies.size(); ++i) {
        const auto& e = m.enemies.enemies[i];
        if (!ValidEnemy(e)) return false;
        for (size_t j = 0; j < i; ++j) if (m.enemies.enemies[j].id == e.id) return false;
        WriteEnemy(payload, e);
      }
      break;
    case MessageType::EnemyEvent:
      if (!ValidEnemyWorld(m.enemy_event.world) || !ValidEnemy(m.enemy_event.enemy) ||
          m.enemy_event.kind < EnemyEventKind::Spawned || m.enemy_event.kind > EnemyEventKind::Removed) return false;
      WriteWorld(payload, m.enemy_event.world); payload.U(uint8_t(m.enemy_event.kind), 1);
      WriteEnemy(payload, m.enemy_event.enemy);
      break;
    case MessageType::ShotReceipt:
      if (!ValidShotReceipt(m.shot_receipt)) return false;
      WriteWorld(payload,m.shot_receipt.world); payload.U(m.shot_receipt.number,4);
      payload.U(uint8_t(m.shot_receipt.reason),1); payload.U(m.shot_receipt.queued,1);
      break;
    case MessageType::ShotIntent:
      if (!ValidShotIntent(m.shot)) return false;
      WriteWorld(payload, m.shot.world); payload.U(m.shot.number, 4); payload.U(m.shot.weapon, 1);
      payload.U(std::bit_cast<uint64_t>(m.shot.time), 8);
      WriteVector(payload, m.shot.origin); WriteVector(payload, m.shot.direction);
      payload.U(m.shot.hits.size(), 1);
      for (const auto& hit : m.shot.hits) {
        payload.U(hit.id.source, 8); payload.U(hit.id.sequence, 4); payload.U(uint16_t(hit.id.family), 2);
        WriteVector(payload, hit.point); payload.U(hit.generation, 4);
      }
      break;
    default: return false;
  }
  Writer packet;
  packet.U(kMagic, 4); packet.U(m.protocol, 2); packet.U(uint16_t(m.type), 2);
  packet.U(m.sequence, 4); packet.U(m.epoch, 8); packet.U(payload.bytes.size(), 4);
  packet.bytes.insert(packet.bytes.end(), payload.bytes.begin(), payload.bytes.end());
  if (packet.bytes.size() > kMaxPacketBytes) return false;
  out = std::move(packet.bytes);
  return true;
}

bool Decode(std::span<const uint8_t> bytes, Message& out) {
  if (bytes.size() < kHeaderBytes || bytes.size() > kMaxPacketBytes) return false;
  Reader r{bytes};
  if (r.U(4) != kMagic) return false;
  Message m;
  m.protocol = uint16_t(r.U(2)); m.type = MessageType(r.U(2));
  m.sequence = uint32_t(r.U(4)); m.epoch = r.U(8);
  if (r.U(4) != bytes.size() - kHeaderBytes) return false;
  switch (m.type) {
    case MessageType::ObjectState: case MessageType::ObjectRequest:
      m.object.generation=uint32_t(r.U(4)); m.object.level=uint32_t(r.U(4)); m.object.number=uint32_t(r.U(4));
      m.object.source=r.U(8); m.object.kind=ObjectKind(r.U(1)); m.object.value=uint32_t(r.U(4));
      m.object.position=std::bit_cast<float>(uint32_t(r.U(4))); m.object.seconds=std::bit_cast<float>(uint32_t(r.U(4)));
      if (!(m.type==MessageType::ObjectRequest ? ValidObjectRequest(m.object) : ValidObjectState(m.object))) return false;
      break;
    case MessageType::WorldDigest: {
      auto& p=m.digest;
      p.generation=uint32_t(r.U(4)); p.level=uint32_t(r.U(4)); p.start=uint32_t(r.U(4));
      p.checkpoint=uint32_t(r.U(4)); p.sweep=uint32_t(r.U(4)); p.captured=std::bit_cast<double>(r.U(8));
      p.enemy_world=ReadWorld(r); p.completed=r.U(8);
      p.part=uint16_t(r.U(2)); p.parts=uint16_t(r.U(2)); p.script_count=uint16_t(r.U(2)); p.enemy_count=uint16_t(r.U(2));
      p.extensions=uint32_t(r.U(4)); const size_t scripts=r.U(1),enemies=r.U(1);
      if (scripts>kDigestScriptsPerPage || enemies>kDigestEnemiesPerPage) return false;
      for (size_t i=0;i<scripts;++i) {
        WorldScript s; s.source=r.U(8); s.script=r.U(8); s.run=uint32_t(r.U(4)); s.record=uint32_t(r.U(4)); s.flags=uint8_t(r.U(1));
        p.scripts.push_back(s);
      }
      for (size_t i=0;i<enemies;++i) {
        WorldEnemy e; e.id={r.U(8),uint32_t(r.U(4)),EnemyFamily(r.U(2))};
        e.owner=EnemyOwner(r.U(1)); e.generation=uint32_t(r.U(4));
        e.life=std::bit_cast<float>(uint32_t(r.U(4))); e.secondary_life=std::bit_cast<float>(uint32_t(r.U(4)));
        p.enemies.push_back(e);
      }
      if (!ValidWorldDigest(p)) return false;
      break;
    }
    case MessageType::StoryEvent: case MessageType::TriggerRequest:
      m.story.generation=uint32_t(r.U(4)); m.story.level=uint32_t(r.U(4)); m.story.number=uint32_t(r.U(4));
      m.story.source=r.U(8); m.story.script=r.U(8);
      m.story.run=uint32_t(r.U(4)); m.story.record=uint32_t(r.U(4)); m.story.phase=StoryPhase(r.U(1));
      if (!ValidStoryEvent(m.story) || (m.type == MessageType::TriggerRequest && !ValidStoryRequest(m.story.phase)) ||
          (m.type == MessageType::StoryEvent && m.story.phase==StoryPhase::SnapshotRequest)) return false;
      break;
    case MessageType::OwnershipChange:
      m.ownership.world = ReadWorld(r); m.ownership.phase = OwnershipPhase(r.U(1));
      m.ownership.generation = uint32_t(r.U(4)); m.ownership.owner = EnemyOwner(r.U(1));
      m.ownership.enemy = ReadEnemy(r);
      if (!ValidOwnershipChange(m.ownership)) return false;
      break;
    case MessageType::PlayerDowned: case MessageType::PlayerRevived:
      m.life.world = {uint32_t(r.U(4)), uint32_t(r.U(4)), uint32_t(r.U(4)), r.U(8)};
      m.life.incarnation = r.U(8); m.life.number = uint32_t(r.U(4)); m.life.checkpoint = uint32_t(r.U(4));
      if (!ValidPlayerLife(m.life)) return false;
      break;
    case MessageType::CameraPose: case MessageType::CameraCut: {
      auto& p=m.camera;
      p.generation=uint32_t(r.U(4)); p.level=uint32_t(r.U(4));
      p.cut=uint32_t(r.U(4)); p.skip=uint32_t(r.U(4)); p.time=std::bit_cast<double>(r.U(8));
      for (float* f : {&p.x,&p.y,&p.z,&p.qx,&p.qy,&p.qz,&p.qw,&p.fov})
        *f=std::bit_cast<float>(uint32_t(r.U(4)));
      const auto flags=r.U(1); if (flags>3) return false;
      p.scripted=(flags&1)!=0; p.paused=(flags&2)!=0;
      if (!ValidCameraPose(p)) return false;
      break;
    }
    case MessageType::DamageEvent:
      m.damage.world = ReadWorld(r); m.damage.incarnation = r.U(8); m.damage.number = uint32_t(r.U(4));
      m.damage.enemy = {r.U(8), uint32_t(r.U(4)), EnemyFamily(r.U(2))};
      m.damage.flags = uint32_t(r.U(4)); m.damage.magnitude = uint32_t(r.U(4)); m.damage.direction = ReadVector(r);
      m.damage.time = std::bit_cast<double>(r.U(8));
      if (!ValidDamageEvent(m.damage)) return false;
      break;
    case MessageType::PlayerStatus:
      m.player_status.world = ReadWorld(r); m.player_status.incarnation = r.U(8);
      m.player_status.number = uint32_t(r.U(4)); m.player_status.last_damage = uint32_t(r.U(4));
      m.player_status.condition = PlayerCondition(r.U(1));
      if (!ValidPlayerStatus(m.player_status)) return false;
      break;
    case MessageType::Cheats:
      m.cheats.bits = uint16_t(r.U(2));
      if (!ValidCheatSet(m.cheats)) return false;
      break;
    case MessageType::InventoryState:
      m.inventory.world = ReadWorld(r); m.inventory.time = std::bit_cast<double>(r.U(8));
      m.inventory.weapon = uint8_t(r.U(1));
      for (auto& ammo : m.inventory.ammo) ammo = uint32_t(r.U(4));
      if (!ValidInventory(m.inventory)) return false;
      break;
    case MessageType::SessionStart: {
      m.start.chapter = uint16_t(r.U(2)); m.start.level = uint32_t(r.U(4)); m.start.start = uint32_t(r.U(4));
      const auto chapter = r.U(1); if (chapter > 1) return false;
      m.start.chapter_select = chapter != 0; m.start.cheats = uint16_t(r.U(2)); m.start.difficulty = uint8_t(r.U(1));
      m.start.save_bytes = uint32_t(r.U(4)); m.start.save_hash = r.U(8);
      if (!ValidSessionStart(m.start)) return false;
      break;
    }
    case MessageType::SaveChunk: {
      m.save_chunk.offset = uint32_t(r.U(4)); const size_t count = size_t(r.U(2));
      if (!r.ok || !count || count > kSaveChunkBytes || count != bytes.size() - r.at ||
          m.save_chunk.offset > kSessionSaveBytes || count > kSessionSaveBytes - m.save_chunk.offset) return false;
      m.save_chunk.bytes.assign(bytes.begin() + r.at, bytes.end()); r.at += count;
      break;
    }
    case MessageType::LevelChange: case MessageType::Loaded: case MessageType::LoadRelease:
    case MessageType::Checkpoint: case MessageType::ChapterFinished: case MessageType::ReloadRequest:
    case MessageType::FlowAbort:
      m.flow = {uint32_t(r.U(4)), uint32_t(r.U(4)), uint32_t(r.U(4)), r.U(8)};
      if (!ValidFlowEvent(m.flow)) return false;
      break;
    case MessageType::Hello: {
      m.hello.protocol = uint16_t(r.U(2));
      if (!r.Text(m.hello.version) || !r.Text(m.hello.build) || !r.Text(m.hello.gameplay_digest)) return false;
      const size_t n = size_t(r.U(1));
      if (!r.ok || !n || n > kMaxDisplayNameBytes || n > bytes.size()-r.at) return false;
      m.display_name.assign(reinterpret_cast<const char*>(bytes.data()+r.at), n);
      r.at += n;
      if (SanitizeDisplayName(m.display_name) != m.display_name) return false;
      if (r.at < bytes.size()) {
        const size_t code_size = size_t(r.U(1));
        if (!r.ok || code_size != 6 || code_size > bytes.size()-r.at) return false;
        m.join_code.assign(reinterpret_cast<const char*>(bytes.data()+r.at), code_size);
        r.at += code_size;
        for (unsigned char c : m.join_code) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
      }
      break;
    }
    case MessageType::Reject:
      m.reason = RejectReason(r.U(2));
      if (!ReasonValid(m.reason)) return false;
      break;
    case MessageType::Ping: case MessageType::Pong: m.clock_ms = r.U(8); break;
    case MessageType::PlayerPose:
      m.pose.time = std::bit_cast<double>(r.U(8)); m.pose.level = uint32_t(r.U(4));
      m.pose.x = std::bit_cast<float>(uint32_t(r.U(4)));
      m.pose.y = std::bit_cast<float>(uint32_t(r.U(4)));
      m.pose.z = std::bit_cast<float>(uint32_t(r.U(4)));
      m.pose.facing = std::bit_cast<float>(uint32_t(r.U(4)));
      m.pose.speed = std::bit_cast<float>(uint32_t(r.U(4)));
      m.pose.action = uint16_t(r.U(2));
      m.pose.item = uint8_t(r.U(1));
      m.pose.vx = std::bit_cast<float>(uint32_t(r.U(4)));
      m.pose.vy = std::bit_cast<float>(uint32_t(r.U(4)));
      m.pose.pitch = std::bit_cast<float>(uint32_t(r.U(4)));
      m.pose.weapon = uint8_t(r.U(1));
      { const auto stance=r.U(1); if (stance>3) return false;
        m.pose.crouched=uint8_t(stance&1); m.pose.airborne=uint8_t(stance>>1); }
      m.pose.fire = uint32_t(r.U(4)); m.pose.fire_weapon = uint8_t(r.U(1));
      if (!ValidPose(m.pose)) return false;
      break;
    case MessageType::HelloAck: case MessageType::Bye: break;
    case MessageType::EnemySnapshot: {
      m.enemies.world = ReadWorld(r); m.enemies.time = std::bit_cast<double>(r.U(8));
      const size_t count = size_t(r.U(2));
      if (!ValidEnemyWorld(m.enemies.world) || !std::isfinite(m.enemies.time) || m.enemies.time < 0 ||
          m.enemies.time >= 1e12 || count > kMaxEnemyBatch || count * kEnemyStateBytes != bytes.size() - r.at) return false;
      for (size_t i = 0; i < count; ++i) {
        auto e = ReadEnemy(r); if (!ValidEnemy(e)) return false;
        for (const auto& prior : m.enemies.enemies) if (prior.id == e.id) return false;
        m.enemies.enemies.push_back(e);
      }
      break;
    }
    case MessageType::EnemyEvent:
      m.enemy_event.world = ReadWorld(r); m.enemy_event.kind = EnemyEventKind(r.U(1));
      m.enemy_event.enemy = ReadEnemy(r);
      if (!ValidEnemyWorld(m.enemy_event.world) || !ValidEnemy(m.enemy_event.enemy) ||
          m.enemy_event.kind < EnemyEventKind::Spawned || m.enemy_event.kind > EnemyEventKind::Removed) return false;
      break;
    case MessageType::ShotReceipt: {
      m.shot_receipt.world=ReadWorld(r); m.shot_receipt.number=uint32_t(r.U(4));
      m.shot_receipt.reason=ShotReject(r.U(1)); const auto queued=r.U(1);
      if (queued>1 || !ValidShotReceipt(m.shot_receipt)) return false;
      m.shot_receipt.queued=queued!=0; break;
    }
    case MessageType::ShotIntent: {
      m.shot.world = ReadWorld(r); m.shot.number = uint32_t(r.U(4)); m.shot.weapon = uint8_t(r.U(1));
      m.shot.time = std::bit_cast<double>(r.U(8)); m.shot.origin = ReadVector(r); m.shot.direction = ReadVector(r);
      const size_t count = size_t(r.U(1));
      if (!r.ok || count > kMaxShotHits || count * 30 != bytes.size()-r.at) return false;
      for (size_t i = 0; i < count; ++i) {
        ShotHit hit; hit.id = {r.U(8), uint32_t(r.U(4)), EnemyFamily(r.U(2))}; hit.point = ReadVector(r);
        hit.generation = uint32_t(r.U(4));
        m.shot.hits.push_back(hit);
      }
      if (!ValidShotIntent(m.shot)) return false;
      break;
    }
    default: return false;
  }
  if (!r.ok || r.at != bytes.size()) return false;
  out = std::move(m);
  return true;
}

RejectReason CheckCompatibility(const Compatibility& a, const Compatibility& b) {
  if (a.protocol != b.protocol) return RejectReason::Protocol;
  if (a.version != b.version) return RejectReason::Version;
  if (a.build != b.build) return RejectReason::Build;
  if (a.gameplay_digest != b.gameplay_digest) return RejectReason::GameplayMods;
  return RejectReason::None;
}
}  // namespace kk::coop
