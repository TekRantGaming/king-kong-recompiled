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
    case RejectReason::Protocol: return "different co-op version; both players need the same build";
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
    p.vx += (b.vx - a.vx) * t; p.vy += (b.vy - a.vy) * t;
    p.pitch += (b.pitch - a.pitch) * t;
    p.facing += std::remainder(b.facing - a.facing, 6.28318530718f) * t;
    // Discrete events use the same delayed timeline as the rendered body.
    if (t >= .5f) { p.action = b.action; p.item = b.item; p.weapon = b.weapon; p.crouched = b.crouched; p.airborne = b.airborne; }
    if (t >= 1.f) { p.fire = b.fire; p.fire_weapon = b.fire_weapon; }
    return p;
  }
  return poses_.back().pose;  // hold, never extrapolate through a wall
}
std::optional<PlayerPose> PoseBuffer::At(double time, uint32_t level) const {
  if (!std::isfinite(time)) return {};
  const PlayerPose* nearest = nullptr;
  for (const auto& entry : poses_) {
    const auto& p = entry.pose;
    if (p.level != level) continue;
    if (!nearest || std::abs(p.time-time) < std::abs(nearest->time-time)) nearest = &p;
  }
  // Use the sender's clock only. A WAN shot must not be compared with the
  // render-delayed current pose after the player has moved away from it.
  if (!nearest || std::abs(nearest->time-time) > .5) return {};
  return *nearest;
}
CoopSession::CoopSession(SessionConfig c, ICoopTransport& t, StateCallback f)
    : config_(std::move(c)), transport_(t), changed_(std::move(f)), epoch_(config_.epoch) {
  config_.display_name = SanitizeDisplayName(config_.display_name);
}
void CoopSession::Transition(SessionState s) {
  if (state_ == s) return;
  state_ = s;
  if (s != SessionState::Connected) {
    cameras_.Clear(); local_camera_.reset(); sent_camera_cut_.reset();
    poses_.Clear(); enemies_.Clear(); outgoing_shots_.clear(); incoming_shots_.clear();
    outgoing_receipts_.clear(); incoming_receipts_.clear();
    remote_inventory_.reset(); sent_cheats_.reset();
    local_inventory_.reset(); sent_inventory_.reset();
    outgoing_combat_.clear(); incoming_combat_.clear();
    outgoing_ownership_.clear(); incoming_ownership_.clear();
    outgoing_story_.clear(); incoming_story_.clear();
    outgoing_objects_.clear(); incoming_objects_.clear();
    // Keep host settings during a temporary loss. End/leave restores own choices.
    if (s != SessionState::Lost) host_cheats_.reset();
    if (s == SessionState::Ended) { outgoing_flow_.clear(); incoming_flow_.clear(); }
  }
  if (s == SessionState::Ended) partner_name_.clear();
  if (changed_) changed_(s);
}
bool CoopSession::Send(Message m, bool reliable) {
  if (m.type == MessageType::Hello) { m.display_name = config_.display_name; m.join_code = config_.join_code; }
  m.protocol = config_.compatibility.protocol;
  m.sequence = ++sequence_;
  m.epoch = epoch_;
  std::vector<uint8_t> bytes;
  if (!Encode(m, bytes) || !transport_.Send(bytes, reliable)) return false;
  if (m.type == MessageType::PlayerPose) ++pose_counters_.sent;
  if (m.type == MessageType::EnemySnapshot) enemies_sent_ += m.enemies.enemies.size();
  if (m.type == MessageType::ShotIntent) ++shots_sent_;
  return true;
}
void CoopSession::Receive(const Message& m, double now) {
  const bool camera=m.type==MessageType::CameraPose || m.type==MessageType::CameraCut;
  auto reject_camera=[&](const char* why) {
    ++camera_info_.rejected_count; camera_info_.rejected_at=now; camera_info_.rejection=why;
  };
  if (camera) { camera_info_.received=now; camera_info_.sequence=m.sequence; }
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
    partner_name_ = m.display_name;
    hello_seen_ = true;
    Message ack; ack.type = MessageType::HelloAck; Send(ack, true);
    last_receive_ = now;
  } else if (m.type == MessageType::Reject && state_ == SessionState::Connecting) {
    if (m.epoch != epoch_ || !epoch_) return;
    rejection_ = m.reason; Transition(SessionState::Ended); return;
  } else {
    if (!epoch_ || m.epoch != epoch_ || m.protocol != config_.compatibility.protocol) {
      if (camera) reject_camera("wrong epoch/protocol");
      return;
    }
    if (m.type == MessageType::HelloAck && state_ == SessionState::Connecting) {
      ack_seen_ = true;
    } else {
      if (state_ != SessionState::Connected && state_ != SessionState::Lost) {
        if (camera) reject_camera("session not connected");
        return;
      }
      switch (m.type) {
        case MessageType::ObjectState: case MessageType::ObjectRequest:
          if (state_!=SessionState::Connected || ObjectSender(m.type,config_.role)) return;
          if (incoming_objects_.size()>=kObjectQueue) { End(now); return; }
          incoming_objects_.push_back(m); break;
        case MessageType::StoryEvent: case MessageType::TriggerRequest: case MessageType::WorldDigest:
          if (state_!=SessionState::Connected || StorySender(m.type,config_.role)) return;
          if (!RetainStoryMessage(incoming_story_,m)) { End(now); return; }
          break;
        case MessageType::OwnershipChange:
          if (state_ != SessionState::Connected || OwnershipSender(m.ownership.phase, config_.role)) return;
          if (incoming_ownership_.size() >= kMaxEnemies*2) { End(now); return; }
          incoming_ownership_.push_back(m); break;
        case MessageType::CameraPose: case MessageType::CameraCut:
          if (config_.role!=Role::Guest || state_!=SessionState::Connected) {
            reject_camera("wrong role/session state"); return;
          }
          if (camera_seen_ && int32_t(m.sequence-camera_sequence_)<=0) {
            reject_camera("old/duplicate sequence"); return;
          }
          if (!cameras_.Push(m.camera,now)) { reject_camera(cameras_.Rejection()); return; }
          ++camera_info_.accepted_count; camera_info_.accepted=now; camera_info_.accepted_sequence=m.sequence;
          camera_seen_=true; camera_sequence_=m.sequence; break;
        case MessageType::DamageEvent: case MessageType::PlayerStatus:
          if (state_ != SessionState::Connected ||
              (m.type == MessageType::DamageEvent ? config_.role != Role::Guest : config_.role != Role::Host)) return;
          if (incoming_combat_.size() >= combat::kQueueLimit) { End(now); return; }
          incoming_combat_.push_back({m, now}); break;
        case MessageType::Cheats:
          if (config_.role != Role::Guest || (cheat_seen_ && int32_t(m.sequence-cheat_sequence_) <= 0)) return;
          cheat_seen_ = true; cheat_sequence_ = m.sequence; host_cheats_ = m.cheats; break;
        case MessageType::InventoryState:
          if (state_ != SessionState::Connected ||
              (inventory_seen_ && int32_t(m.sequence-inventory_sequence_) <= 0)) return;
          inventory_seen_ = true; inventory_sequence_ = m.sequence; remote_inventory_ = m.inventory; break;
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
        case MessageType::EnemySnapshot: case MessageType::EnemyEvent:
          if (state_ != SessionState::Connected) return;
          if (m.type == MessageType::EnemyEvent) {
            if ((m.enemy_event.enemy.owner == EnemyOwner::Host) == (config_.role == Role::Host)) return;
          } else for (const auto& e : m.enemies.enemies)
            if ((e.owner == EnemyOwner::Host) == (config_.role == Role::Host)) return;
          enemies_.Receive(m, now); break;
        case MessageType::ShotReceipt:
          if (state_!=SessionState::Connected || config_.role!=Role::Guest) return;
          if (incoming_receipts_.size()<kMaxQueuedShots) incoming_receipts_.push_back(m.shot_receipt);
          else ++malformed_;
          break;
        case MessageType::ShotIntent:
          if (state_ != SessionState::Connected) return;
          // The game-thread validator owns sequence rejection and its reason
          // counter. Never silently apply or discard a replay in a second policy.
          if (incoming_shots_.size() < kMaxQueuedShots)
            incoming_shots_.push_back({m.shot, now, remote_inventory_, poses_.At(m.shot.time, m.shot.world.level)});
          else ++malformed_;
          break;
        default:
          if (!IsFlowMessage(m.type) || incoming_flow_.size() >= 64) return;
          incoming_flow_.push_back(m);
          break;
      }
    }
    last_receive_ = now;
  }
  if (state_ == SessionState::Connecting && hello_seen_ && ack_seen_) Transition(SessionState::Connected);
  else if (state_ == SessionState::Lost) Transition(SessionState::Connected);
}
void CoopSession::Tick(double now, const std::optional<PlayerPose>& local, const enemies::Frame* frame, const CheatSet* cheats) {
  // Disabled is inert, including callbacks, polling, reads, writes and transport state queries.
  if (!config_.enabled || state_ == SessionState::Ended) return;
  if (cheats && ValidCheatSet(*cheats)) config_.cheats = *cheats;
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
  const bool waiting = config_.wait_for_peer && config_.role == Role::Host &&
      state_ == SessionState::Connecting && transport_.Stats().received == 0;
  if (waiting) started_ = now;
  if (now - (state_ == SessionState::Connecting ? started_ : last_receive_) >= 20) {
    End(now); return;
  }
  if (state_ == SessionState::Connecting) {
    if (!waiting && now - last_hello_ >= .5) {
      Message hello; hello.type = MessageType::Hello; hello.hello = config_.compatibility;
      // A guest always initiates with epoch zero, even after learning the host's epoch.
      const uint64_t saved = epoch_;
      if (config_.role == Role::Guest) epoch_ = 0;
      Send(hello, true); epoch_ = saved; last_hello_ = now;
    }
    return;
  }
  if (now - last_receive_ >= 3) Transition(SessionState::Lost);
  if (state_==SessionState::Connected && config_.role==Role::Host && local_camera_ &&
      now>=local_camera_->time && now-local_camera_->time<=.25 && ValidCameraPose(*local_camera_)) {
    const auto& p=*local_camera_;
    if (!sent_camera_cut_ || p.generation!=sent_camera_cut_->generation || p.cut!=sent_camera_cut_->cut ||
        p.skip!=sent_camera_cut_->skip || p.paused!=sent_camera_cut_->paused) {
      Message m; m.type=MessageType::CameraCut; m.camera=p;
      if (Send(m,true)) sent_camera_cut_=p;
    }
    if (now-last_camera_>=1.0/30.0) {
      Message m; m.type=MessageType::CameraPose; m.camera=p;
      if (Send(m,false)) last_camera_=now;
    }
  }
  if (state_ == SessionState::Connected && config_.role == Role::Host && sent_cheats_ != config_.cheats) {
    Message m; m.type = MessageType::Cheats; m.cheats = config_.cheats;
    if (Send(m, true)) sent_cheats_ = config_.cheats;
  }
  if (now - last_ping_ >= 1) {
    Message ping; ping.type = MessageType::Ping;
    ping.clock_ms = ping_stamp_ = uint64_t(now * 1000); ping_pending_ = true;
    Send(ping, false); last_ping_ = now;
  }
  if (state_ == SessionState::Connected) for (int n = 0; n < 8 && !outgoing_flow_.empty(); ++n) {
    if (!Send(outgoing_flow_.front(), true)) break;
    outgoing_flow_.pop_front();
  }
  if (state_ == SessionState::Connected) for (unsigned n = 0; n < 16 && !outgoing_ownership_.empty(); ++n) {
    const auto& m = outgoing_ownership_.front();
    if (!Send(m, true)) break;
    // The host also switches on the successfully sent Commit, through the same
    // game-thread message path as the guest. Queueing a packet is not a grant.
    if (m.ownership.phase == OwnershipPhase::Commit) {
      if (incoming_ownership_.size() >= kMaxEnemies*2) { End(now); return; }
      incoming_ownership_.push_back(m);
    }
    outgoing_ownership_.pop_front();
  }
  if (state_==SessionState::Connected) for (unsigned n=0; n<32 && !outgoing_objects_.empty(); ++n) {
    if (!Send(outgoing_objects_.front(),true)) break;
    outgoing_objects_.pop_front();
  }
  if (state_==SessionState::Connected) for (unsigned n=0; n<32 && !outgoing_story_.empty(); ++n) {
    if (!Send(outgoing_story_.front(),true)) break;
    outgoing_story_.pop_front();
  }
  if (state_ == SessionState::Connected) for (unsigned n = 0; n < 8 && !outgoing_combat_.empty(); ++n) {
    const auto& entry = outgoing_combat_.front();
    if (entry.message.epoch != epoch_ || !combat::Fresh(entry.received, now)) { outgoing_combat_.pop_front(); continue; }
    if (!Send(entry.message, true)) break;
    outgoing_combat_.pop_front();
  }
  if (local && now - last_pose_ >= .05 && state_ == SessionState::Connected) {
    Message pose; pose.type = MessageType::PlayerPose; pose.pose = *local;
    Send(pose, false); last_pose_ = now;
  }
  if (state_ == SessionState::Connected) {
    const bool fresh = frame && now-frame->time >= 0 && now-frame->time <= .25 && ValidEnemyWorld(frame->world);
    enemies::Frame owned;
    if (fresh) {
      owned = *frame;
      std::erase_if(owned.records, [&](const auto& r) {
        return (r.enemy.owner == EnemyOwner::Host) != (config_.role == Role::Host);
      });
      enemies_.Publish(owned);
    }
    for (int i = 0; i < 8 && enemies_.Event(); ++i) {
      Message event; event.type = MessageType::EnemyEvent; event.enemy_event = *enemies_.Event();
      if (!Send(event, true)) break;
      enemies_.SentEvent();
    }
    if (fresh && !owned.records.empty() && now-last_enemies_ >= .08) {
      for (auto& batch : enemies_.Snapshots()) {
        Message snapshot; snapshot.type = MessageType::EnemySnapshot; snapshot.enemies = std::move(batch);
        Send(snapshot, false);
      }
      last_enemies_ = now;
    }
  }
  if (state_ == SessionState::Connected) {
    if (local_inventory_ && now-local_inventory_->time >= 0 && now-local_inventory_->time <= .5 &&
        (!sent_inventory_ || !SameInventory(*local_inventory_, *sent_inventory_))) {
      Message m; m.type = MessageType::InventoryState; m.inventory = *local_inventory_;
      if (Send(m, true)) sent_inventory_ = local_inventory_;
    }
    unsigned sent = 0;
    while (sent < 8 && !outgoing_shots_.empty()) {
      const auto& pending = outgoing_shots_.front();
      Message m; m.type = MessageType::ShotIntent; m.shot = pending.intent;
      // Local steady clocks match between the worker and game thread.
      if (now-m.shot.time > .5) { outgoing_shots_.pop_front(); continue; }
      Message inventory; inventory.type = MessageType::InventoryState; inventory.inventory = pending.inventory;
      if (!Send(inventory, true)) break;
      sent_inventory_ = pending.inventory;
      if (!Send(m, true)) break;
      outgoing_shots_.pop_front(); ++sent;
    }
    for (unsigned n=0;n<8 && !outgoing_receipts_.empty();++n) {
      Message receipt; receipt.type=MessageType::ShotReceipt; receipt.shot_receipt=outgoing_receipts_.front();
      if (!Send(receipt,true)) break;
      outgoing_receipts_.pop_front();
    }
  }
}
bool CoopSession::QueueShot(const PendingShot& shot) {
  if (state_ != SessionState::Connected ||
      !ValidPendingShot(shot) || outgoing_shots_.size() >= kMaxQueuedShots) return false;
  outgoing_shots_.push_back(shot); return true;
}
bool CoopSession::QueueShotReceipt(const ShotReceipt& receipt) {
  if (state_!=SessionState::Connected || config_.role!=Role::Host || !ValidShotReceipt(receipt) ||
      outgoing_receipts_.size()>=kMaxQueuedShots) return false;
  outgoing_receipts_.push_back(receipt); return true;
}
void CoopSession::PublishInventory(const InventoryState& inventory) {
  if (state_ == SessionState::Connected && ValidInventory(inventory)) local_inventory_ = inventory;
}
bool CoopSession::QueueOwnership(const Message& m) {
  if (state_ != SessionState::Connected || m.type != MessageType::OwnershipChange || m.epoch != epoch_ ||
      !ValidOwnershipChange(m.ownership) || !OwnershipSender(m.ownership.phase, config_.role) ||
      outgoing_ownership_.size() >= kMaxEnemies*2) return false;
  outgoing_ownership_.push_back(m); return true;
}
std::vector<Message> CoopSession::TakeOwnership() {
  auto out = std::move(incoming_ownership_); incoming_ownership_.clear(); return out;
}
bool CoopSession::QueueFlow(const Message& m) {
  if (state_ != SessionState::Connected || !IsFlowMessage(m.type) || outgoing_flow_.size() >= 64) return false;
  std::vector<uint8_t> bytes; if (!Encode(m, bytes)) return false;
  outgoing_flow_.push_back(m); return true;
}
bool CoopSession::QueueObjects(const Message& m) {
  if (state_!=SessionState::Connected || !IsObjectMessage(m.type) || m.epoch!=epoch_ ||
      !ObjectSender(m.type,config_.role) || outgoing_objects_.size()>=kObjectQueue) return false;
  std::vector<uint8_t> bytes; if (!Encode(m,bytes)) return false;
  outgoing_objects_.push_back(m); return true;
}
std::vector<Message> CoopSession::TakeObjects() {
  auto out=std::move(incoming_objects_); incoming_objects_.clear(); return out;
}
bool CoopSession::QueueStory(const Message& m) {
  if (state_!=SessionState::Connected || !IsStoryMessage(m.type) || m.epoch!=epoch_ ||
      !StorySender(m.type,config_.role) || outgoing_story_.size()>=kStoryQueue) return false;
  if (m.type==MessageType::WorldDigest && outgoing_story_.size()>=kStoryQueue/4) return false;
  std::vector<uint8_t> bytes; if (!Encode(m,bytes)) return false;
  outgoing_story_.push_back(m); return true;
}
std::vector<Message> CoopSession::TakeStory() {
  auto out=std::move(incoming_story_); incoming_story_.clear(); return out;
}
bool CoopSession::QueueCombat(const Message& m, double submitted) {
  if (state_ != SessionState::Connected || !IsCombatMessage(m.type) || m.epoch != epoch_ ||
      !std::isfinite(submitted) || outgoing_combat_.size() >= combat::kQueueLimit ||
      (m.type == MessageType::DamageEvent ? config_.role != Role::Host : config_.role != Role::Guest)) return false;
  std::vector<uint8_t> bytes; if (!Encode(m, bytes)) return false;
  outgoing_combat_.push_back({m, submitted}); return true;
}
std::vector<ReceivedCombat> CoopSession::TakeCombat() {
  auto out = std::move(incoming_combat_); incoming_combat_.clear(); return out;
}
std::vector<Message> CoopSession::TakeFlow() { auto out = std::move(incoming_flow_); incoming_flow_.clear(); return out; }
std::vector<ReceivedShot> CoopSession::TakeShots() { auto out = std::move(incoming_shots_); incoming_shots_.clear(); return out; }
std::vector<ShotReceipt> CoopSession::TakeShotReceipts() { auto out=std::move(incoming_receipts_); incoming_receipts_.clear(); return out; }
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
