#pragma once

#include <cstdint>
#include <compare>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "coop_cheats.h"

namespace kk::coop {
constexpr uint32_t kMagic = 0x4B4B4331;  // KKC1, network byte order throughout
constexpr uint16_t kProtocolVersion = 8;
constexpr size_t kMaxDisplayNameBytes = 32;
// Valid UTF-8, no controls or invisible direction/line formatting, bounded at a codepoint.
// Empty or wholly invalid input becomes "Partner". Never log the input or result.
std::string SanitizeDisplayName(std::string_view name);
constexpr size_t kHeaderBytes = 24, kMaxPacketBytes = 1024;
enum class MessageType : uint16_t {
  Hello = 1, HelloAck = 2, Reject = 3, Ping = 4, Pong = 5, PlayerPose = 6, Bye = 7,
  EnemySnapshot = 8, EnemyEvent = 9, ShotIntent = 10, Cheats = 11, InventoryState = 12,
  SessionStart = 13, SaveChunk = 14, LevelChange = 15, Loaded = 16, LoadRelease = 17,
  Checkpoint = 18, ChapterFinished = 19, ReloadRequest = 20, FlowAbort = 21,
  DamageEvent = 22, PlayerStatus = 23, OwnershipChange = 24,
  PlayerDowned = 25, PlayerRevived = 26
};

// Session control is reliable and scoped by the packet epoch and a load generation.
// Saves are bounded opaque game data, never addresses or paths supplied by a peer.
constexpr uint32_t kSessionSaveBytes = 0x8040, kSaveChunkBytes = 768;
struct SessionStart {
  uint16_t chapter = 0;
  uint32_t level = 100, start = 0;
  bool chapter_select = true;
  uint16_t cheats = 0;  // bit 8: enabled; bits 0..7: healing, one-hit, ammo, spears, four guns
  uint8_t difficulty = 0;  // 0 normal, 1 port hardcore
  uint32_t save_bytes = 0;
  uint64_t save_hash = 14695981039346656037ull;  // FNV-1a of empty data, integrity only
};
struct FlowEvent {
  uint32_t generation = 0, level = 0, start = 0;
  uint64_t completed = 0;  // chapter entries completed in this session only
};
// Life observations use the shared load generation, not native pointers or the
// number of private reloads. Both peers publish a reliable heartbeat every .5 s.
struct PlayerLife {
  FlowEvent world;
  uint64_t incarnation = 0;
  uint32_t number = 0, checkpoint = 0;
};
bool ValidPlayerLife(const PlayerLife& life);
inline bool IsLifeMessage(MessageType t) {
  return t == MessageType::PlayerDowned || t == MessageType::PlayerRevived;
}
struct SaveChunk { uint32_t offset = 0; std::vector<uint8_t> bytes; };
bool ValidSessionStart(const SessionStart& start);
bool ValidFlowEvent(const FlowEvent& event);
uint64_t SessionSaveHash(std::span<const uint8_t> bytes);
bool IsFlowMessage(MessageType type);
enum class RejectReason : uint16_t { None, Protocol, Version, Build, GameplayMods, Handshake };
struct Compatibility {
  uint16_t protocol = kProtocolVersion;
  std::string version, build, gameplay_digest;
};
struct PlayerPose {
  double time = 0;
  uint32_t level = 0;
  float x = 0, y = 0, z = 0, facing = 0, speed = 0;
  uint16_t action = 0;
  uint8_t item = 0;  // Diagnostic only: first-person IDs are not body animation IDs.
};
enum class EnemyFamily : uint16_t { None, Raptor, Worm, Scorpion };
enum class EnemyOwner : uint8_t { Host, Guest };
// source is a signature of family + authored spawn point, or spawner + template.
// sequence is zero for authored actors, successful creation number for waves.
// No component contains a guest address. IDs are scoped to EnemyWorld + session epoch.
struct EnemyId {
  uint64_t source = 0;
  uint32_t sequence = 0;
  EnemyFamily family = EnemyFamily::None;
  auto operator<=>(const EnemyId&) const = default;
};
struct EnemyWorld {
  // serial counts visits to this level/start while enabled, excluding menu loads.
  uint32_t level = 0, start = 0, serial = 0;
  auto operator<=>(const EnemyWorld&) const = default;
};
struct EnemyState {
  EnemyId id;
  float x = 0, y = 0, z = 0, facing = 0, life = 0;
  uint32_t state = 0;  // Compiled current state function, diagnostic, never invoked from wire.
  uint16_t action = 0;
  uint8_t item = 0;
  float spawn_x = 0, spawn_y = 0, spawn_z = 0; // Immutable identity evidence, never latest XYZ.
  EnemyOwner owner = EnemyOwner::Host;
  uint32_t generation = 0; // Authority generation, independent of actor/spawn sequence.
};
// Prepare freezes both copies; Ready returns the old owner's final pose. Only
// the host's Commit changes authority. Matched confirms a usable guest binding.
enum class OwnershipPhase : uint8_t { Matched = 1, Prepare, Ready, Commit };
struct OwnershipChange {
  EnemyWorld world;
  OwnershipPhase phase = OwnershipPhase::Matched;
  uint32_t generation = 0;
  EnemyOwner owner = EnemyOwner::Host; // proposed/new owner
  EnemyState enemy; // last state, tagged with the OLD owner/generation until Commit
};
bool ValidOwnershipChange(const OwnershipChange& change);
constexpr size_t kMaxEnemyBatch = 16, kMaxEnemies = 512;

struct ShotVector { float x = 0, y = 0, z = 0; };
enum class PlayerCondition : uint8_t { Alive = 1, Hurt, Dead };
// Header epoch scopes the session. World includes the checkpoint visit; incarnation
// changes on each local life/load. Values only, never native addresses or handles.
struct DamageEvent {
  EnemyWorld world;
  uint64_t incarnation = 0;
  uint32_t number = 0;
  EnemyId enemy;
  uint32_t flags = 0, magnitude = 0; // native payload +88/+92 are integer words
  ShotVector direction;
  double time = 0; // host pose clock, bounds reliable transport delay as well as mailbox age
};
struct PlayerStatus {
  EnemyWorld world;
  uint64_t incarnation = 0;
  uint32_t number = 0, last_damage = 0; // processed, not necessarily injured
  PlayerCondition condition = PlayerCondition::Alive;
};
bool ValidDamageEvent(const DamageEvent& damage);
bool ValidPlayerStatus(const PlayerStatus& status);
inline bool IsCombatMessage(MessageType t) {
  return t == MessageType::DamageEvent || t == MessageType::PlayerStatus;
}
struct ShotHit { EnemyId id; ShotVector point; uint32_t generation = 0; };
struct InventoryState {
  EnemyWorld world;
  double time = 0;
  uint8_t weapon = 0;
  uint32_t ammo[4]{};
};
bool ValidInventory(const InventoryState& inventory);
inline bool SameInventory(const InventoryState& a, const InventoryState& b) {
  if (a.world != b.world || a.weapon != b.weapon) return false;
  for (unsigned i = 0; i < 4; ++i) if (a.ammo[i] != b.ammo[i]) return false;
  return true;
}
// One normal firearm callback, including misses. Shotgun can hit several distinct actors.
// No damage value, bone index, function or guest pointer is accepted from a peer.
struct ShotIntent {
  EnemyWorld world;
  uint32_t number = 0;
  uint8_t weapon = 0;
  double time = 0;
  ShotVector origin, direction;
  std::vector<ShotHit> hits;
};
constexpr size_t kMaxShotHits = 16, kMaxQueuedShots = 32;
bool ValidShotIntent(const ShotIntent& shot);
enum class EnemyEventKind : uint8_t { Spawned = 1, Died, Removed };
struct EnemySnapshot {
  EnemyWorld world;
  double time = 0;
  std::vector<EnemyState> enemies;
};
struct EnemyEvent {
  EnemyWorld world;
  EnemyEventKind kind = EnemyEventKind::Spawned;
  EnemyState enemy;
};
bool ValidEnemy(const EnemyState& enemy);
bool ValidEnemyWorld(const EnemyWorld& world);
struct Message {
  MessageType type = MessageType::Hello;
  uint16_t protocol = kProtocolVersion;
  uint32_t sequence = 0;
  uint64_t epoch = 0;
  Compatibility hello;
  std::string display_name = "Partner";  // Hello only, separate from compatibility/lobby metadata.
  RejectReason reason = RejectReason::None;
  uint64_t clock_ms = 0;
  PlayerPose pose;
  EnemySnapshot enemies;
  EnemyEvent enemy_event;
  ShotIntent shot;
  DamageEvent damage;
  PlayerStatus player_status;
  OwnershipChange ownership;
  CheatSet cheats;
  InventoryState inventory;
  SessionStart start;
  SaveChunk save_chunk;
  FlowEvent flow;
  PlayerLife life;
};
// Encoders also reject invalid input. Decoding leaves out untouched on failure.
bool Encode(const Message& message, std::vector<uint8_t>& out);
bool Decode(std::span<const uint8_t> bytes, Message& out);
RejectReason CheckCompatibility(const Compatibility& local, const Compatibility& remote);
}  // namespace kk::coop
