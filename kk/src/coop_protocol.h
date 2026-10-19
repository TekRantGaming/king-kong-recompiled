#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace kk::coop {
constexpr uint32_t kMagic = 0x4B4B4331;  // KKC1, network byte order throughout
constexpr uint16_t kProtocolVersion = 1;
constexpr size_t kHeaderBytes = 24, kMaxPacketBytes = 1024;
enum class MessageType : uint16_t { Hello = 1, HelloAck, Reject, Ping, Pong, PlayerPose, Bye };
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
};
struct Message {
  MessageType type = MessageType::Hello;
  uint16_t protocol = kProtocolVersion;
  uint32_t sequence = 0;
  uint64_t epoch = 0;
  Compatibility hello;
  RejectReason reason = RejectReason::None;
  uint64_t clock_ms = 0;
  PlayerPose pose;
};
// Encoders also reject invalid input. Decoding leaves out untouched on failure.
bool Encode(const Message& message, std::vector<uint8_t>& out);
bool Decode(std::span<const uint8_t> bytes, Message& out);
RejectReason CheckCompatibility(const Compatibility& local, const Compatibility& remote);
}  // namespace kk::coop
