#pragma once

#include "coop_protocol.h"
#include "coop_transport.h"
#include "coop_enemies.h"
#include "coop_shots.h"

#include <deque>
#include <functional>
#include <optional>
#include <mutex>

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
  CheatSet cheats;
};
struct PoseCounters {
  uint64_t sent = 0, received = 0;
  std::optional<uint32_t> remote_level;
};
class CoopSession {
 public:
  using StateCallback = std::function<void(SessionState)>;
  CoopSession(SessionConfig config, ICoopTransport& transport, StateCallback changed = {});
  // Deterministic clock in seconds. Runtime calls on its worker; unit tests use an in-memory transport.
  void Tick(double now, const std::optional<PlayerPose>& local = {}, const enemies::Frame* frame = nullptr,
      const CheatSet* cheats = nullptr);
  void End(double now);
  SessionState State() const { return state_; }
  Role SessionRole() const { return config_.role; }
  TransportStats Stats() const;
  RejectReason Rejection() const { return rejection_; }
  std::optional<PlayerPose> Partner(double now) const;
  PoseCounters Poses() const { return pose_counters_; }
  enemies::Frame Enemies(double now) const { return state_ == SessionState::Connected ? enemies_.Sample(now) : enemies::Frame{}; }
  uint64_t EnemiesSent() const { return enemies_sent_; }
  bool QueueShot(const PendingShot& shot);
  void PublishInventory(const InventoryState& inventory);
  std::optional<CheatSet> HostCheats() const { return host_cheats_; }
  std::vector<ReceivedShot> TakeShots();
  uint64_t ShotsSent() const { return shots_sent_; }
 private:
  void Transition(SessionState state);
  bool Send(Message message, bool reliable);
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
  PoseCounters pose_counters_;
  enemies::Channel enemies_;
  double last_enemies_ = -1;
  uint64_t enemies_sent_ = 0;
  std::deque<PendingShot> outgoing_shots_;
  std::vector<ReceivedShot> incoming_shots_;
  uint64_t shots_sent_ = 0;
  std::optional<CheatSet> host_cheats_, sent_cheats_;
  std::optional<InventoryState> remote_inventory_;
  std::optional<InventoryState> local_inventory_, sent_inventory_;
  uint32_t cheat_sequence_ = 0, inventory_sequence_ = 0;
  bool cheat_seen_ = false, inventory_seen_ = false;
};
// Both workers share the same bounded value-only handoff. No guest memory here.
class ShotMailbox {
 public:
  bool Submit(const PendingShot& shot) {
    std::lock_guard lock(mutex_);
    if (!ValidPendingShot(shot) || outgoing_.size() >= kMaxQueuedShots) return false;
    outgoing_.push_back(shot); return true;
  }
  void PublishInventory(const InventoryState& inventory) {
    if (!ValidInventory(inventory)) return;
    std::lock_guard lock(mutex_); inventory_ = inventory;
  }
  std::vector<ReceivedShot> Take() {
    std::lock_guard lock(mutex_); auto out = std::move(incoming_); incoming_.clear(); return out;
  }
  void Clear() { std::lock_guard lock(mutex_); outgoing_.clear(); incoming_.clear(); inventory_.reset(); }
  void Exchange(CoopSession& session) {
    if (session.State() != SessionState::Connected) { Clear(); return; }
    std::lock_guard lock(mutex_);
    if (inventory_) { session.PublishInventory(*inventory_); inventory_.reset(); }
    while (!outgoing_.empty() && session.QueueShot(outgoing_.front())) outgoing_.pop_front();
    for (auto& shot : session.TakeShots()) if (incoming_.size() < kMaxQueuedShots) incoming_.push_back(std::move(shot));
  }
 private:
  std::mutex mutex_;
  std::deque<PendingShot> outgoing_;
  std::vector<ReceivedShot> incoming_;
  std::optional<InventoryState> inventory_;
};
}  // namespace kk::coop
