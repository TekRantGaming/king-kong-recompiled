#include "coop_session.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace kk::coop {
const char* StateName(SessionState s) {
  switch (s) {
    case SessionState::Idle: return "idle";
    case SessionState::Connecting: return "connecting";
    case SessionState::Connected: return "connected";
    case SessionState::Lost: return "connection lost";
    case SessionState::Ended: return "ended";
  }
  return "unknown";
}
const char* RejectName(RejectReason r) {
  switch (r) {
    case RejectReason::None: return "";
    case RejectReason::Protocol: return "different protocol";
    case RejectReason::Version: return "different game version";
    case RejectReason::Build: return "different build";
    case RejectReason::GameplayMods: return "different gameplay mods";
    case RejectReason::Handshake: return "handshake failed";
  }
  return "unknown rejection";
}
void PoseBuffer::Push(PlayerPose p, double received) {
  if (!poses_.empty()) {
    if (p.time <= poses_.back().pose.time) return;
    if (p.level != poses_.back().pose.level) poses_.clear();
  }
  poses_.push_back({p, received});
  while (poses_.size() > 32) poses_.pop_front();
}
std::optional<PlayerPose> PoseBuffer::Sample(double now) const {
  if (poses_.empty() || now - poses_.back().received > .5) return {};
  // Anchor remote time to its latest arrival; no assumption of synchronized process clocks.
  const double target = poses_.back().pose.time + (now - poses_.back().received) - .1;
  if (target <= poses_.front().pose.time) return poses_.front().pose;
  for (size_t i = 1; i < poses_.size(); ++i) {
    const auto& a = poses_[i - 1].pose;
    const auto& b = poses_[i].pose;
    if (target > b.time) continue;
    const float t = float(std::clamp((target - a.time) / (b.time - a.time), 0.0, 1.0));
    PlayerPose p = a;
    p.time = target;
    p.x += (b.x - a.x) * t; p.y += (b.y - a.y) * t; p.z += (b.z - a.z) * t;
    p.speed += (b.speed - a.speed) * t;
    p.facing += std::remainder(b.facing - a.facing, 6.28318530718f) * t;
    if (t >= .5f) { p.action = b.action; p.item = b.item; }
    return p;
  }
  return poses_.back().pose;  // hold, never extrapolate through a wall
}
CoopSession::CoopSession(SessionConfig c, ICoopTransport& t, StateCallback f)
    : config_(std::move(c)), transport_(t), changed_(std::move(f)), epoch_(config_.epoch) {}
void CoopSession::Transition(SessionState s) {
  if (state_ == s) return;
  state_ = s;
  if (s != SessionState::Connected) poses_.Clear();
  if (changed_) changed_(s);
}
void CoopSession::Send(Message m, bool reliable) {
  m.protocol = config_.compatibility.protocol;
  m.sequence = ++sequence_;
  m.epoch = epoch_;
  std::vector<uint8_t> bytes;
  if (Encode(m, bytes) && transport_.Send(bytes, reliable) && m.type == MessageType::PlayerPose)
    ++pose_counters_.sent;
}
void CoopSession::Receive(const Message& m, double now) {
  if (m.type == MessageType::Hello && state_ == SessionState::Connecting) {
    rejection_ = m.protocol != m.hello.protocol ? RejectReason::Protocol :
        CheckCompatibility(config_.compatibility, m.hello);
    const bool epoch_ok = config_.role == Role::Host ? m.epoch == 0 : m.epoch != 0;
    if (!epoch_ok) return;
    if (config_.role == Role::Guest) epoch_ = m.epoch;
    if (rejection_ != RejectReason::None) {
      Message reject; reject.type = MessageType::Reject; reject.reason = rejection_;
      Send(reject, true); Transition(SessionState::Ended); return;
    }
    hello_seen_ = true;
    Message ack; ack.type = MessageType::HelloAck; Send(ack, true);
    last_receive_ = now;
  } else if (m.type == MessageType::Reject && state_ == SessionState::Connecting) {
    if (m.epoch != epoch_ || !epoch_) return;
    rejection_ = m.reason; Transition(SessionState::Ended); return;
  } else {
    if (!epoch_ || m.epoch != epoch_ || m.protocol != config_.compatibility.protocol) return;
    if (m.type == MessageType::HelloAck && state_ == SessionState::Connecting) {
      ack_seen_ = true;
    } else {
      if (state_ != SessionState::Connected && state_ != SessionState::Lost) return;
      switch (m.type) {
        case MessageType::Ping: {
          Message pong; pong.type = MessageType::Pong; pong.clock_ms = m.clock_ms; Send(pong, false); break;
        }
        case MessageType::Pong:
          if (!ping_pending_ || m.clock_ms != ping_stamp_) return;
          ping_ms_ = std::max(0.0, now * 1000 - double(m.clock_ms)); ping_pending_ = false; break;
        case MessageType::PlayerPose:
          if (pose_seen_ && int32_t(m.sequence - pose_sequence_) <= 0) return;
          pose_seen_ = true; pose_sequence_ = m.sequence;
          ++pose_counters_.received; pose_counters_.remote_level = m.pose.level;
          poses_.Push(m.pose, now); break;
        case MessageType::Bye: Transition(SessionState::Ended); return;
        default: return;
      }
    }
    last_receive_ = now;
  }
  if (state_ == SessionState::Connecting && hello_seen_ && ack_seen_) Transition(SessionState::Connected);
  else if (state_ == SessionState::Lost) Transition(SessionState::Connected);
}
void CoopSession::Tick(double now, const std::optional<PlayerPose>& local) {
  // Disabled is inert, including callbacks, polling, reads, writes and transport state queries.
  if (!config_.enabled || state_ == SessionState::Ended) return;
  if (state_ == SessionState::Idle) {
    if (config_.role == Role::Host && !epoch_) { Transition(SessionState::Ended); return; }
    started_ = last_receive_ = now; Transition(SessionState::Connecting);
    // Queue Hello before reading anything: ordered reliable transports then deliver Hello before Ack.
    Message hello; hello.type = MessageType::Hello; hello.hello = config_.compatibility;
    Send(hello, true); last_hello_ = now;
  }
  transport_.Poll(now);
  if (transport_.State() != TransportState::Ready) { Transition(SessionState::Ended); return; }
  std::vector<uint8_t> bytes;
  for (int n = 0; n < 64 && transport_.Receive(bytes); ++n) {
    Message m;
    if (!Decode(bytes, m)) { ++malformed_; continue; }
    Receive(m, now);
    if (state_ == SessionState::Ended) return;
  }
  if (now - (state_ == SessionState::Connecting ? started_ : last_receive_) >= 20) {
    End(now); return;
  }
  if (state_ == SessionState::Connecting) {
    if (now - last_hello_ >= .5) {
      Message hello; hello.type = MessageType::Hello; hello.hello = config_.compatibility;
      // A guest always initiates with epoch zero, even after learning the host's epoch.
      const uint64_t saved = epoch_;
      if (config_.role == Role::Guest) epoch_ = 0;
      Send(hello, true); epoch_ = saved; last_hello_ = now;
    }
    return;
  }
  if (now - last_receive_ >= 3) Transition(SessionState::Lost);
  if (now - last_ping_ >= 1) {
    Message ping; ping.type = MessageType::Ping;
    ping.clock_ms = ping_stamp_ = uint64_t(now * 1000); ping_pending_ = true;
    Send(ping, false); last_ping_ = now;
  }
  if (local && now - last_pose_ >= .05 && state_ == SessionState::Connected) {
    Message pose; pose.type = MessageType::PlayerPose; pose.pose = *local;
    Send(pose, false); last_pose_ = now;
  }
}
void CoopSession::End(double) {
  if (!config_.enabled || state_ == SessionState::Idle || state_ == SessionState::Ended) return;
  Message bye; bye.type = MessageType::Bye; Send(bye, true);
  Transition(SessionState::Ended);
}
TransportStats CoopSession::Stats() const {
  if (!config_.enabled) return {};
  auto s = transport_.Stats();
  if (ping_ms_ > 0) s.ping_ms = ping_ms_;
  s.malformed += malformed_; return s;
}
std::optional<PlayerPose> CoopSession::Partner(double now) const {
  return state_ == SessionState::Connected ? poses_.Sample(now) : std::nullopt;
}
}  // namespace kk::coop
