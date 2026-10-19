#include "steam_transport.h"
#include "coop_protocol.h"

namespace kk::coop {
namespace {
bool Valid(std::span<const uint8_t> bytes, bool reliable) {
  Message message;
  if (bytes.size() > kMaxPacketBytes || !Decode(bytes, message)) return false;
  if (message.type == MessageType::PlayerPose || message.type == MessageType::EnemySnapshot) return !reliable;
  // C03 intentionally sends Ping/Pong unreliably. Preserve that existing transport contract.
  if (message.type == MessageType::Ping || message.type == MessageType::Pong) return true;
  return reliable;
}
}
SteamMessagesTransport::SteamMessagesTransport(ISteamMessagesBackend& backend, uint64_t partner)
    : backend_(backend), partner_(partner) { if (!partner) state_ = TransportState::Failed; }
SteamMessagesTransport::~SteamMessagesTransport() { Close(); }
bool SteamMessagesTransport::Send(std::span<const uint8_t> bytes, bool reliable) {
  if (state_ != TransportState::Ready || !Valid(bytes, reliable)) return false;
  if (!backend_.Send(partner_, reliable ? 0 : 1, bytes, reliable)) { state_ = TransportState::Failed; return false; }
  ++stats_.sent; return true;
}
bool SteamMessagesTransport::Receive(std::vector<uint8_t>& bytes) {
  if (state_ != TransportState::Ready || incoming_.empty()) return false;
  bytes = std::move(incoming_.front()); incoming_.pop_front(); return true;
}
void SteamMessagesTransport::Poll(double) {
  if (state_ != TransportState::Ready) return;
  // Bound both work and memory even if a permitted peer floods us.
  for (int channel = 0; channel < 2; ++channel) for (unsigned n = 0; n < 32; ++n) {
    SteamBackendMessage m;
    if (!backend_.Receive(channel, m)) break;
    if (m.peer != partner_ || m.channel != channel || m.oversized || m.reliable != (channel == 0) || !Valid(m.bytes, m.reliable)) {
      ++stats_.malformed; continue;
    }
    if (incoming_.size() >= 64) {
      if (m.reliable) { state_ = TransportState::Failed; return; }
      continue;
    }
    incoming_.push_back(std::move(m.bytes)); ++stats_.received;
  }
}
TransportStats SteamMessagesTransport::Stats() const {
  auto s = stats_;
  if (state_ == TransportState::Ready) { const auto b = backend_.Stats(partner_); s.ping_ms = b.ping_ms; s.loss = b.loss; }
  return s;
}
void SteamMessagesTransport::Close() {
  if (state_ == TransportState::Closed) return;
  if (partner_) backend_.Close(partner_);
  incoming_.clear(); state_ = TransportState::Closed;
}
bool SteamMessagesTransport::SessionRequest(uint64_t peer) {
  if (!peer) return false;
  if (state_ != TransportState::Ready || peer != partner_) { backend_.Close(peer); return false; }
  if (backend_.Accept(peer)) return true;
  state_ = TransportState::Failed; return false;
}
void SteamMessagesTransport::SessionFailed(uint64_t peer) { if (peer == partner_) state_ = TransportState::Failed; }
}  // namespace kk::coop
