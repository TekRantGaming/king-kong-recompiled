#include "coop_protocol.h"

#include <bit>
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
      std::abs(p.speed) < 10000 && std::abs(p.facing) <= 3.1416f;
}
bool ReasonValid(RejectReason r) { return r >= RejectReason::Protocol && r <= RejectReason::Handshake; }
void WriteWorld(Writer& w, const EnemyWorld& v) { w.U(v.level, 4); w.U(v.start, 4); w.U(v.serial, 4); }
EnemyWorld ReadWorld(Reader& r) { return {uint32_t(r.U(4)), uint32_t(r.U(4)), uint32_t(r.U(4))}; }
void WriteEnemy(Writer& w, const EnemyState& e) {
  w.U(e.id.source, 8); w.U(e.id.sequence, 4); w.U(uint16_t(e.id.family), 2);
  for (float v : {e.x, e.y, e.z, e.facing, e.life}) w.U(std::bit_cast<uint32_t>(v), 4);
  w.U(e.state, 4); w.U(e.action, 2); w.U(e.item, 1);
  for (float v : {e.spawn_x, e.spawn_y, e.spawn_z}) w.U(std::bit_cast<uint32_t>(v), 4);
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
uint64_t SessionSaveHash(std::span<const uint8_t> bytes) {
  uint64_t hash = 14695981039346656037ull;
  for (uint8_t b : bytes) { hash ^= b; hash *= 1099511628211ull; }
  return hash;
}
bool IsFlowMessage(MessageType t) { return t >= MessageType::SessionStart && t <= MessageType::FlowAbort; }
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
bool ValidEnemy(const EnemyState& e) {
  return e.id.source && e.id.family >= EnemyFamily::Raptor && e.id.family <= EnemyFamily::Scorpion &&
      std::isfinite(e.x) && std::isfinite(e.y) && std::isfinite(e.z) && std::isfinite(e.facing) &&
      std::isfinite(e.life) && std::abs(e.x) < 1000000 && std::abs(e.y) < 1000000 &&
      std::abs(e.z) < 1000000 && std::abs(e.facing) <= 3.1416f && std::abs(e.life) < 1000000 &&
      Point({e.spawn_x, e.spawn_y, e.spawn_z});
}
bool ValidShotIntent(const ShotIntent& s) {
  if (!ValidEnemyWorld(s.world) || !s.number || s.weapon < 1 || s.weapon > 4 ||
      !std::isfinite(s.time) || s.time < 0 || s.time >= 1e12 || !Point(s.origin) || !Point(s.direction) ||
      s.hits.size() > (s.weapon == 3 ? kMaxShotHits : 1)) return false;
  const float length = std::hypot(s.direction.x, s.direction.y, s.direction.z);
  if (length < .99f || length > 1.01f) return false;
  for (size_t i = 0; i < s.hits.size(); ++i) {
    const auto& hit = s.hits[i];
    if (!hit.id.source || hit.id.family < EnemyFamily::Raptor || hit.id.family > EnemyFamily::Scorpion || !Point(hit.point)) return false;
    for (size_t j = 0; j < i; ++j) if (s.hits[j].id == hit.id) return false;
  }
  return true;
}

bool Encode(const Message& m, std::vector<uint8_t>& out) {
  Writer payload;
  switch (m.type) {
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
    case MessageType::ShotIntent:
      if (!ValidShotIntent(m.shot)) return false;
      WriteWorld(payload, m.shot.world); payload.U(m.shot.number, 4); payload.U(m.shot.weapon, 1);
      payload.U(std::bit_cast<uint64_t>(m.shot.time), 8);
      WriteVector(payload, m.shot.origin); WriteVector(payload, m.shot.direction);
      payload.U(m.shot.hits.size(), 1);
      for (const auto& hit : m.shot.hits) {
        payload.U(hit.id.source, 8); payload.U(hit.id.sequence, 4); payload.U(uint16_t(hit.id.family), 2);
        WriteVector(payload, hit.point);
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
    case MessageType::Hello:
      m.hello.protocol = uint16_t(r.U(2));
      if (!r.Text(m.hello.version) || !r.Text(m.hello.build) || !r.Text(m.hello.gameplay_digest)) return false;
      break;
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
      if (!ValidPose(m.pose)) return false;
      break;
    case MessageType::HelloAck: case MessageType::Bye: break;
    case MessageType::EnemySnapshot: {
      m.enemies.world = ReadWorld(r); m.enemies.time = std::bit_cast<double>(r.U(8));
      const size_t count = size_t(r.U(2));
      if (!ValidEnemyWorld(m.enemies.world) || !std::isfinite(m.enemies.time) || m.enemies.time < 0 ||
          m.enemies.time >= 1e12 || count > kMaxEnemyBatch || count * 53 != bytes.size() - r.at) return false;
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
    case MessageType::ShotIntent: {
      m.shot.world = ReadWorld(r); m.shot.number = uint32_t(r.U(4)); m.shot.weapon = uint8_t(r.U(1));
      m.shot.time = std::bit_cast<double>(r.U(8)); m.shot.origin = ReadVector(r); m.shot.direction = ReadVector(r);
      const size_t count = size_t(r.U(1));
      if (!r.ok || count > kMaxShotHits || count * 26 != bytes.size()-r.at) return false;
      for (size_t i = 0; i < count; ++i) {
        ShotHit hit; hit.id = {r.U(8), uint32_t(r.U(4)), EnemyFamily(r.U(2))}; hit.point = ReadVector(r);
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
