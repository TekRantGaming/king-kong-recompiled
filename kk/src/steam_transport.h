#pragma once
#include "coop_transport.h"

#include <deque>

namespace kk::coop {
// SDK adapter copies at most kMaxPacketBytes and releases every SDK message before returning.
struct SteamBackendMessage {
  uint64_t peer = 0;
  int channel = 0;
  bool reliable = false, oversized = false;
  std::vector<uint8_t> bytes;
};
class ISteamMessagesBackend {
 public:
  virtual ~ISteamMessagesBackend() = default;
  virtual bool Send(uint64_t peer, int channel, std::span<const uint8_t> bytes, bool reliable) = 0;
  virtual bool Receive(int channel, SteamBackendMessage& out) = 0;
  virtual bool Accept(uint64_t peer) = 0;
  virtual void Close(uint64_t peer) = 0;
  virtual TransportStats Stats(uint64_t peer) const = 0;
};
// All methods and backend calls belong to the same Steam owner thread.
class SteamMessagesTransport final : public ICoopTransport {
 public:
  SteamMessagesTransport(ISteamMessagesBackend& backend, uint64_t partner);
  ~SteamMessagesTransport() override;
  bool Send(std::span<const uint8_t> bytes, bool reliable) override;
  bool Receive(std::vector<uint8_t>& bytes) override;
  void Poll(double now) override;
  TransportState State() const override { return state_; }
  TransportStats Stats() const override;
  void Close() override;
  bool SessionRequest(uint64_t peer);
  void SessionFailed(uint64_t peer);
 private:
  ISteamMessagesBackend& backend_;
  uint64_t partner_;
  TransportState state_ = TransportState::Ready;
  TransportStats stats_;
  std::deque<std::vector<uint8_t>> incoming_;
};
}  // namespace kk::coop
