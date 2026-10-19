#pragma once

#include <cstdint>
#include <compare>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace kk::coop {
constexpr uint32_t kMagic = 0x4B4B4331;  // KKC1, network byte order throughout
constexpr uint16_t kProtocolVersion = 3;
constexpr size_t kHeaderBytes = 24, kMaxPacketBytes = 1024;
enum class MessageType : uint16_t { Hello = 1, HelloAck, Reject, Ping, Pong, PlayerPose, Bye,
  EnemySnapshot, EnemyEvent };
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
  // serial counts visits to this level/start while Connected, excluding menu loads.
  uint32_t level = 0, start = 0, serial = 0;
  auto operator<=>(const EnemyWorld&) const = default;
};
struct EnemyState {
  EnemyId id;
  float x = 0, y = 0, z = 0, facing = 0, life = 0;
  uint32_t state = 0;  // Compiled current state function, diagnostic, never invoked from wire.
  uint16_t action = 0;
  uint8_t item = 0;
};
constexpr size_t kMaxEnemyBatch = 20, kMaxEnemies = 512;
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
  RejectReason reason = RejectReason::None;
  uint64_t clock_ms = 0;
  PlayerPose pose;
  EnemySnapshot enemies;
  EnemyEvent enemy_event;
};
// Encoders also reject invalid input. Decoding leaves out untouched on failure.
bool Encode(const Message& message, std::vector<uint8_t>& out);
bool Decode(std::span<const uint8_t> bytes, Message& out);
RejectReason CheckCompatibility(const Compatibility& local, const Compatibility& remote);
}  // namespace kk::coop
