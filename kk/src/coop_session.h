#pragma once

#include "coop_protocol.h"
#include "coop_transport.h"
#include "coop_enemies.h"
#include "coop_shots.h"
#include "coop_combat.h"
#include "coop_camera.h"

#include <deque>
#include <algorithm>
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
  std::optional<PlayerPose> At(double time, uint32_t level) const;
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
  std::string display_name = "Partner";
  std::string join_code;
  bool wait_for_peer = false;  // Direct IP host may wait in its lobby until a guest arrives.
};
struct PoseCounters {
  uint64_t sent = 0, received = 0;
  std::optional<uint32_t> remote_level;
};
struct ReceivedCombat { Message message; double received = 0; };
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
  uint64_t Epoch() const { return epoch_; }
  TransportStats Stats() const;
  RejectReason Rejection() const { return rejection_; }
  std::optional<PlayerPose> Partner(double now) const;
  PoseCounters Poses() const { return pose_counters_; }
  const std::string& PartnerName() const { return partner_name_; }
  enemies::Frame Enemies(double now) const { return state_ == SessionState::Connected ? enemies_.Sample(now) : enemies::Frame{}; }
  uint64_t EnemiesSent() const { return enemies_sent_; }
  bool QueueShot(const PendingShot& shot);
  void PublishInventory(const InventoryState& inventory);
  std::optional<CheatSet> HostCheats() const { return host_cheats_; }
  std::vector<ReceivedShot> TakeShots();
  bool QueueShotReceipt(const ShotReceipt& receipt);
  std::vector<ShotReceipt> TakeShotReceipts();
  uint64_t ShotsSent() const { return shots_sent_; }
  bool QueueFlow(const Message& message);
  std::vector<Message> TakeFlow();
  bool QueueCombat(const Message& message, double submitted);
  std::vector<ReceivedCombat> TakeCombat();
  bool QueueOwnership(const Message& message);
  std::vector<Message> TakeOwnership();
  bool QueueStory(const Message& message);
  std::vector<Message> TakeStory();
  void PublishCamera(std::optional<CameraPose> camera) { local_camera_=std::move(camera); }
  std::optional<CameraPose> HostCamera(double now) const {
    return state_==SessionState::Connected && config_.role==Role::Guest ? cameras_.Sample(now) : std::nullopt;
  }
  CameraDiagnostics CameraInfo() const { return camera_info_; }
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
  std::string partner_name_;
  enemies::Channel enemies_;
  double last_enemies_ = -1;
  uint64_t enemies_sent_ = 0;
  std::deque<PendingShot> outgoing_shots_;
  std::vector<ReceivedShot> incoming_shots_;
  std::deque<ShotReceipt> outgoing_receipts_;
  std::vector<ShotReceipt> incoming_receipts_;
  uint64_t shots_sent_ = 0;
  std::optional<CheatSet> host_cheats_, sent_cheats_;
  std::optional<InventoryState> remote_inventory_;
  std::optional<InventoryState> local_inventory_, sent_inventory_;
  uint32_t cheat_sequence_ = 0, inventory_sequence_ = 0;
  bool cheat_seen_ = false, inventory_seen_ = false;
  std::deque<Message> outgoing_flow_;
  std::vector<Message> incoming_flow_;
  std::deque<ReceivedCombat> outgoing_combat_;
  std::vector<ReceivedCombat> incoming_combat_;
  std::deque<Message> outgoing_ownership_;
  std::vector<Message> incoming_ownership_;
  std::deque<Message> outgoing_story_;
  std::vector<Message> incoming_story_;
  std::optional<CameraPose> local_camera_, sent_camera_cut_;
  CameraBuffer cameras_;
  CameraDiagnostics camera_info_;
  double last_camera_=-1;
  uint32_t camera_sequence_=0;
  bool camera_seen_=false;
};
inline bool OwnershipSender(OwnershipPhase phase, Role role) {
  return (phase == OwnershipPhase::Prepare || phase == OwnershipPhase::Commit) == (role == Role::Host);
}
inline bool StorySender(MessageType type, Role role) {
  return (type==MessageType::StoryEvent && role==Role::Host) ||
      (type==MessageType::TriggerRequest && role==Role::Guest);
}
class StoryMailbox {
 public:
  bool Submit(Message m) {
    if (!IsStoryMessage(m.type) || !m.epoch || !ValidStoryEvent(m.story) ||
        (m.type==MessageType::TriggerRequest && !ValidStoryRequest(m.story.phase))) return false;
    std::lock_guard lock(mutex_);
    if (outgoing_.size()>=kStoryQueue) return false;
    outgoing_.push_back(std::move(m)); return true;
  }
  std::vector<Message> Take() {
    std::lock_guard lock(mutex_); auto out=std::move(incoming_); incoming_.clear(); return out;
  }
  void Clear() { std::lock_guard lock(mutex_); outgoing_.clear(); incoming_.clear(); }
  void Exchange(CoopSession& session) {
    if (session.State()!=SessionState::Connected) { Clear(); return; }
    std::lock_guard lock(mutex_);
    while (!outgoing_.empty()) {
      const auto& m=outgoing_.front();
      if (m.epoch!=session.Epoch() || !StorySender(m.type,session.SessionRole())) { outgoing_.pop_front(); continue; }
      if (!session.QueueStory(m)) break;
      outgoing_.pop_front();
    }
    for (auto& m : session.TakeStory()) {
      if (incoming_.size()>=kStoryQueue) { session.End(0); incoming_.clear(); outgoing_.clear(); break; }
      incoming_.push_back(std::move(m));
    }
  }
 private:
  std::mutex mutex_;
  std::deque<Message> outgoing_;
  std::vector<Message> incoming_;
};
class OwnershipMailbox {
 public:
  bool Submit(Message m) {
    if (m.type != MessageType::OwnershipChange || !m.epoch || !ValidOwnershipChange(m.ownership)) return false;
    std::lock_guard lock(mutex_);
    if (outgoing_.size() >= kMaxEnemies*2) return false;
    outgoing_.push_back(std::move(m)); return true;
  }
  std::vector<Message> Take() {
    std::lock_guard lock(mutex_); auto out = std::move(incoming_); incoming_.clear(); return out;
  }
  void Clear() { std::lock_guard lock(mutex_); outgoing_.clear(); incoming_.clear(); }
  void Exchange(CoopSession& session) {
    if (session.State() != SessionState::Connected) { Clear(); return; }
    std::lock_guard lock(mutex_);
    while (!outgoing_.empty()) {
      const auto& m = outgoing_.front();
      if (m.epoch != session.Epoch() || !OwnershipSender(m.ownership.phase, session.SessionRole())) {
        outgoing_.pop_front(); continue;
      }
      if (!session.QueueOwnership(m)) break;
      outgoing_.pop_front();
    }
    for (auto& m : session.TakeOwnership()) {
      if (incoming_.size() >= kMaxEnemies*2) { session.End(0); incoming_.clear(); outgoing_.clear(); break; }
      incoming_.push_back(std::move(m));
    }
  }
 private:
  std::mutex mutex_;
  std::deque<Message> outgoing_;
  std::vector<Message> incoming_;
};
// One bounded copied-value slot in each direction. Workers alone access CoopSession.
class CameraMailbox {
 public:
  void Publish(std::optional<CameraPose> p) {
    std::lock_guard lock(mutex_); local_=p && ValidCameraPose(*p) ? p : std::nullopt;
  }
  std::optional<CameraPose> Sample(double now) const {
    std::lock_guard lock(mutex_);
    return now>=updated_ && info_.accepted>=0 && now>=info_.accepted &&
        now-info_.accepted<=kCameraExpirySeconds ? remote_ : std::nullopt;
  }
  CameraDiagnostics Info() const { std::lock_guard lock(mutex_); return info_; }
  void Clear() { std::lock_guard lock(mutex_); local_.reset(); remote_.reset(); info_={}; }
  void Exchange(CoopSession& session,double now) {
    std::lock_guard lock(mutex_);
    info_=session.CameraInfo();
    if (session.State()!=SessionState::Connected) { local_.reset(); remote_.reset(); return; }
    session.PublishCamera(local_ && now>=local_->time && now-local_->time<=.25 ? local_ : std::nullopt);
    remote_=session.HostCamera(now); updated_=now;
  }
 private:
  mutable std::mutex mutex_;
  std::optional<CameraPose> local_,remote_;
  double updated_=0;
  CameraDiagnostics info_;
};
// Same bounded value-only handoff for UDP and Steam. Epoch checked again by the
// session before sending, so old work cannot leak into a replacement connection.
class CombatMailbox {
 public:
  bool Submit(Message m, double now) {
    if (!IsCombatMessage(m.type) || !m.epoch || !std::isfinite(now)) return false;
    std::vector<uint8_t> bytes; if (!Encode(m, bytes)) return false;
    std::lock_guard lock(mutex_);
    if (outgoing_.size() >= combat::kQueueLimit) return false;
    outgoing_.push_back({std::move(m), now}); return true;
  }
  std::vector<ReceivedCombat> Take() {
    std::lock_guard lock(mutex_); auto out = std::move(incoming_); incoming_.clear(); return out;
  }
  void Clear() { std::lock_guard lock(mutex_); outgoing_.clear(); incoming_.clear(); }
  void Exchange(CoopSession& session, double now) {
    if (session.State() != SessionState::Connected) { Clear(); return; }
    std::lock_guard lock(mutex_);
    while (!outgoing_.empty()) {
      const auto& entry = outgoing_.front();
      if (entry.message.epoch != session.Epoch() || !combat::Fresh(entry.received, now) ||
          (entry.message.type == MessageType::DamageEvent ? session.SessionRole() != Role::Host : session.SessionRole() != Role::Guest)) {
        outgoing_.pop_front(); continue;
      }
      if (!session.QueueCombat(entry.message, entry.received)) break;
      outgoing_.pop_front();
    }
    for (auto& entry : session.TakeCombat()) {
      if (entry.message.type == MessageType::PlayerStatus) {
        // Reliable heartbeats keep the lease fresh. A paused game thread needs
        // the latest observation, not an unbounded history of identical states.
        auto prior = std::find_if(incoming_.begin(), incoming_.end(), [&](const auto& old) {
          return old.message.type == MessageType::PlayerStatus && old.message.epoch == entry.message.epoch &&
              old.message.player_status.world == entry.message.player_status.world &&
              old.message.player_status.incarnation == entry.message.player_status.incarnation;
        });
        if (prior != incoming_.end()) {
          const auto& a = prior->message.player_status; const auto& b = entry.message.player_status;
          if (b.number > a.number && b.last_damage >= a.last_damage &&
              (a.condition != PlayerCondition::Dead || b.condition == PlayerCondition::Dead)) *prior = std::move(entry);
          continue;
        }
      }
      if (incoming_.size() >= combat::kQueueLimit) {
        // Reliable backpressure must fail closed instead of silently losing a hit.
        session.End(now); outgoing_.clear(); incoming_.clear(); break;
      }
      incoming_.push_back(std::move(entry));
    }
  }
 private:
  std::mutex mutex_;
  std::deque<ReceivedCombat> outgoing_;
  std::vector<ReceivedCombat> incoming_;
};
// Both workers share the same bounded value-only handoff. No guest memory here.
class ShotMailbox {
 public:
  bool SubmitReceipt(const ShotReceipt& receipt) {
    if (!ValidShotReceipt(receipt)) return false;
    std::lock_guard lock(mutex_);
    if (outgoing_receipts_.size()>=kMaxQueuedShots) return false;
    outgoing_receipts_.push_back(receipt); return true;
  }
  std::vector<ShotReceipt> TakeReceipts() {
    std::lock_guard lock(mutex_); auto out=std::move(incoming_receipts_); incoming_receipts_.clear(); return out;
  }
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
  void Clear() {
    std::lock_guard lock(mutex_); outgoing_.clear(); incoming_.clear(); inventory_.reset();
    outgoing_receipts_.clear(); incoming_receipts_.clear();
  }
  void Exchange(CoopSession& session) {
    if (session.State() != SessionState::Connected) { Clear(); return; }
    std::lock_guard lock(mutex_);
    if (inventory_) { session.PublishInventory(*inventory_); inventory_.reset(); }
    while (!outgoing_.empty() && session.QueueShot(outgoing_.front())) outgoing_.pop_front();
    for (auto& shot : session.TakeShots()) if (incoming_.size() < kMaxQueuedShots) incoming_.push_back(std::move(shot));
    while (!outgoing_receipts_.empty() && session.QueueShotReceipt(outgoing_receipts_.front())) outgoing_receipts_.pop_front();
    for (auto& receipt:session.TakeShotReceipts())
      if (incoming_receipts_.size()<kMaxQueuedShots) incoming_receipts_.push_back(std::move(receipt));
  }
 private:
  std::mutex mutex_;
  std::deque<PendingShot> outgoing_;
  std::vector<ReceivedShot> incoming_;
  std::deque<ShotReceipt> outgoing_receipts_;
  std::vector<ShotReceipt> incoming_receipts_;
  std::optional<InventoryState> inventory_;
};
}  // namespace kk::coop
