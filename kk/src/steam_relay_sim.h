#pragma once
// Local test backend for the real SteamMessagesTransport. No Steam protocol,
// SDK, account or relay service is emulated. The link supplies ordered reliable
// delivery, retransmission and optional datagram faults. This layer models the
// application's Steam channels, size caps, buffering and NoDelay drops.
#include "steam_transport.h"
#include "coop_protocol.h"
#include <algorithm>
#include <array>

namespace kk::coop {
struct SteamRelayOptions {
  size_t send_buffer_bytes = 64 * 1024;
  double bytes_per_second = 16384, latency_seconds = .15, route_seconds = 1.5;
  double transition_seconds = 0;
};
class SteamRelaySimulation final : public ICoopTransport, private ISteamMessagesBackend {
 public:
  SteamRelaySimulation(ICoopTransport& link, SteamRelayOptions options = {})
      : link_(link), options_(options), messages_(*this, 1) {}
  ~SteamRelaySimulation() override { messages_.Close(); }
  bool Send(std::span<const uint8_t> bytes, bool reliable) override { return messages_.Send(bytes,reliable); }
  bool Receive(std::vector<uint8_t>& out) override { return messages_.Receive(out); }
  void Poll(double now) override {
    now_=now;
    if (started_<0) {
      // CoopSession queues its first Hello before the first Poll. Rebase that
      // relative queue onto this process's clock, which can have any epoch.
      started_=now; free_at_+=now;
      for (auto& p:pending_) p.due+=now;
    }
    link_.Poll(now);
    while (!pending_.empty() && pending_.front().due<=now) {
      auto& p=pending_.front();
      if (!link_.Send(p.bytes,p.reliable)) break;
      buffered_-=p.bytes.size(); pending_.pop_front();
    }
    // Both channels share the connection, but only channel 0 is ordered.
    // SteamMessagesTransport itself applies its production 32/channel poll cap.
    std::vector<uint8_t> bytes;
    for (unsigned i=0;i<64 && incoming_[0].size()+incoming_[1].size()<64 && link_.Receive(bytes);++i) {
      Message m;
      if (!Decode(bytes,m)) continue;
      const bool reliable=m.type!=MessageType::PlayerPose && m.type!=MessageType::EnemySnapshot &&
          m.type!=MessageType::CameraPose && m.type!=MessageType::Ping && m.type!=MessageType::Pong;
      incoming_[reliable ? 0 : 1].push_back({1,reliable ? 0 : 1,reliable,false,std::move(bytes)});
    }
    messages_.Poll(now);
  }
  TransportState State() const override {
    return link_.State()==TransportState::Ready ? messages_.State() : link_.State();
  }
  TransportStats Stats() const override { return messages_.Stats(); }
  void Close() override { messages_.Close(); }
  uint64_t Retries() const { return retries_; }
  uint64_t NoDelayDrops() const { return drops_; }
 private:
  SteamSendResult Send(uint64_t, int channel, std::span<const uint8_t> bytes, bool reliable) override {
    if (bytes.size()>kMaxPacketBytes || bytes.size()>512*1024 || channel!=(reliable ? 0 : 1)) return SteamSendResult::Failed;
    const double route=started_<0 ? now_+options_.route_seconds : started_+options_.route_seconds;
    const double begin=std::max({now_,free_at_,route});
    if (!reliable && begin-now_>.2) { ++drops_; return SteamSendResult::Ignored; }
    if (buffered_+bytes.size()>options_.send_buffer_bytes) { ++retries_; return SteamSendResult::Retry; }
    Message m; Decode(bytes,m);
    free_at_=begin+bytes.size()/options_.bytes_per_second;
    // Delay the reliable stream, never reorder it across a delayed LevelChange.
    double due=free_at_+options_.latency_seconds;
    if (m.type==MessageType::LevelChange) due+=options_.transition_seconds;
    if (!pending_.empty()) due=std::max(due,pending_.back().due);
    pending_.push_back({due,reliable,{bytes.begin(),bytes.end()}}); buffered_+=bytes.size();
    return SteamSendResult::Sent;
  }
  bool Receive(int channel, SteamBackendMessage& out) override {
    auto& q=incoming_[channel];
    if (q.empty()) return false;
    out=std::move(q.front()); q.pop_front(); return true;
  }
  bool Accept(uint64_t) override { return true; }
  void Close(uint64_t) override { link_.Close(); pending_.clear(); buffered_=0; }
  TransportStats Stats(uint64_t) const override { return link_.Stats(); }
  struct Pending { double due; bool reliable; std::vector<uint8_t> bytes; };
  ICoopTransport& link_;
  SteamRelayOptions options_;
  SteamMessagesTransport messages_;
  std::deque<Pending> pending_;
  std::array<std::deque<SteamBackendMessage>,2> incoming_;
  double now_=0,started_=-1,free_at_=0;
  size_t buffered_=0;
  uint64_t retries_=0,drops_=0;
};
} // namespace kk::coop
