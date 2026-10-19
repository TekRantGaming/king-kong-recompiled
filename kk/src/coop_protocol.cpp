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
}  // namespace

bool Encode(const Message& m, std::vector<uint8_t>& out) {
  Writer payload;
  switch (m.type) {
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
      break;
    case MessageType::HelloAck: case MessageType::Bye: break;
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
      if (!ValidPose(m.pose)) return false;
      break;
    case MessageType::HelloAck: case MessageType::Bye: break;
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
