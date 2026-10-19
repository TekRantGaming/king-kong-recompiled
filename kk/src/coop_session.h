#pragma once

#include "coop_protocol.h"
#include "coop_transport.h"

#include <deque>
#include <functional>
#include <optional>

namespace kk::coop {
enum class Role { Host, Guest };
enum class SessionState { Idle, Connecting, Connected, Lost, Ended };
const char* StateName(SessionState state);
const char* RejectName(RejectReason reason);
class PoseBuffer {
 public:
  void Push(PlayerPose pose, double received);
  std::optional<PlayerPose> Sample(double now) const;
  void Clear() { poses_.clear(); }
 private:
  struct Entry { PlayerPose pose; double received; };
  std::deque<Entry> poses_;
};
struct SessionConfig {
  bool enabled = false;
  Role role = Role::Host;
  uint64_t epoch = 0;  // host supplies a fresh, nonzero epoch for each session
  Compatibility compatibility;
};
class CoopSession {
 public:
  using StateCallback = std::function<void(SessionState)>;
  CoopSession(SessionConfig config, ICoopTransport& transport, StateCallback changed = {});
  // Deterministic clock in seconds. Runtime calls on its worker; unit tests use an in-memory transport.
  void Tick(double now, const std::optional<PlayerPose>& local = {});
  void End(double now);
  SessionState State() const { return state_; }
  Role SessionRole() const { return config_.role; }
  TransportStats Stats() const;
  RejectReason Rejection() const { return rejection_; }
  std::optional<PlayerPose> Partner(double now) const;
 private:
  void Transition(SessionState state);
  void Send(Message message, bool reliable);
  void Receive(const Message& message, double now);
  SessionConfig config_;
  ICoopTransport& transport_;
  StateCallback changed_;
  SessionState state_ = SessionState::Idle;
  RejectReason rejection_ = RejectReason::None;
  uint64_t epoch_ = 0;
  uint32_t sequence_ = 0, pose_sequence_ = 0;
  bool hello_seen_ = false, ack_seen_ = false, pose_seen_ = false;
  double started_ = 0, last_receive_ = 0, last_hello_ = -1, last_ping_ = -1, last_pose_ = -1;
  uint64_t ping_stamp_ = 0;
  bool ping_pending_ = false;
  double ping_ms_ = 0;
  uint64_t malformed_ = 0;
  PoseBuffer poses_;
};
}  // namespace kk::coop
