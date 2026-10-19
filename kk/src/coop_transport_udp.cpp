// The localhost test link never exists in a shipping build.
#if defined(KK_DEV_TOOLS)
#include "coop_transport.h"
#include "coop_protocol.h"

#include <array>
#include <algorithm>
#include <cerrno>
#include <deque>
#include <map>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace kk::coop {
namespace {
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalid = INVALID_SOCKET;
void CloseSocket(Socket s) { closesocket(s); }
bool WouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
using Socket = int;
constexpr Socket kInvalid = -1;
void CloseSocket(Socket s) { close(s); }
bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
#endif
constexpr uint32_t kWireMagic = 0x4B4B5531;
constexpr size_t kWireHeader = 12, kWindow = 64;
uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Put32(uint8_t* p, uint32_t v) { for (int i = 3; i >= 0; --i) { p[i] = uint8_t(v); v >>= 8; } }
class DevUdp final : public ICoopTransport {
 public:
  DevUdp(bool host, uint16_t port) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return;
    wsa_ = true;
#endif
    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == kInvalid || !port) return;
    sockaddr_in local{};
    local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = host ? htons(port) : 0;
    // No SO_REUSEADDR: two hosts on the same port must fail rather than split packets.
    if (bind(socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) return;
#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(socket_, FIONBIO, &mode) != 0) return;
#else
    const int flags = fcntl(socket_, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_, F_SETFL, flags | O_NONBLOCK) < 0) return;
#endif
    if (!host) {
      peer_ = local; peer_.sin_port = htons(port); has_peer_ = true;
    }
    state_ = TransportState::Ready;
  }
  ~DevUdp() override {
    Close();
#ifdef _WIN32
    if (wsa_) WSACleanup();
#endif
  }
  bool Send(std::span<const uint8_t> data, bool reliable) override {
    if (state_ != TransportState::Ready || data.empty() || data.size() > kMaxPacketBytes) return false;
    if (reliable && pending_.size() >= kWindow) { state_ = TransportState::Failed; return false; }
    const uint32_t seq = reliable ? next_send_++ : 0;
    std::vector<uint8_t> wire(kWireHeader + data.size());
    Put32(wire.data(), kWireMagic); Put32(wire.data() + 4, seq);
    wire[8] = uint8_t(data.size() >> 8); wire[9] = uint8_t(data.size());
    wire[10] = reliable ? 1 : 0;
    std::copy(data.begin(), data.end(), wire.begin() + kWireHeader);
    if (reliable) pending_.emplace(seq, Pending{wire, -1, 0});
    else if (has_peer_) Transmit(wire);
    return true;
  }
  bool Receive(std::vector<uint8_t>& out) override {
    if (ready_.empty()) return false;
    out = std::move(ready_.front()); ready_.pop_front(); return true;
  }
  void Poll(double now) override {
    if (state_ != TransportState::Ready) return;
    // Flush ordered packets deferred by the bounded consumer queue.
    DrainOrdered();
    for (int i = 0; i < 64; ++i) {
      std::array<uint8_t, kMaxPacketBytes + kWireHeader + 1> wire{};
      sockaddr_in from{};
#ifdef _WIN32
      int length = sizeof(from);
#else
      socklen_t length = sizeof(from);
#endif
      const int n = int(recvfrom(socket_, reinterpret_cast<char*>(wire.data()), int(wire.size()), 0,
          reinterpret_cast<sockaddr*>(&from), &length));
      if (n < 0) {
        if (!WouldBlock()) {
#ifdef _WIN32
          if (WSAGetLastError() == WSAEMSGSIZE) { ++stats_.malformed; continue; }
          // ICMP port unreachable while waiting for the other process is not a fatal UDP error.
          if (WSAGetLastError() == WSAECONNRESET) break;
#else
          if (errno == ECONNREFUSED) break;
#endif
          state_ = TransportState::Failed;
        }
        break;
      }
      if (from.sin_family != AF_INET || from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) continue;
      if (has_peer_ && (from.sin_port != peer_.sin_port || from.sin_addr.s_addr != peer_.sin_addr.s_addr)) continue;
      if (n < int(kWireHeader) || n > int(kMaxPacketBytes + kWireHeader) ||
          Get32(wire.data()) != kWireMagic || wire[11] != 0 || wire[10] > 2 ||
          (size_t(wire[8]) << 8 | wire[9]) != size_t(n) - kWireHeader) {
        ++stats_.malformed; continue;
      }
      const uint32_t seq = Get32(wire.data() + 4);
      const uint8_t kind = wire[10];
      const std::span<const uint8_t> payload(wire.data() + kWireHeader, size_t(n) - kWireHeader);
      if ((kind == 2 && (!payload.empty() || !seq)) || (kind != 2 && payload.empty()) ||
          (kind == 0 && seq) || (kind == 1 && !seq)) { ++stats_.malformed; continue; }
      if (!has_peer_) {
        Message hello;
        // Only a valid first reliable guest Hello can claim a host socket.
        if (kind != 1 || seq != 1 || !Decode(payload, hello) || hello.type != MessageType::Hello || hello.epoch)
          { ++stats_.malformed; continue; }
        peer_ = from; has_peer_ = true;
      }
      ++stats_.received;
      if (kind == 2) {
        const auto pending = pending_.find(seq);
        if (pending != pending_.end()) {
          if (pending->second.last >= 0 && pending->second.attempts == 1) {
            const double rtt = (now - pending->second.last) * 1000;
            stats_.ping_ms = stats_.ping_ms > 0 ? .8 * stats_.ping_ms + .2 * rtt : rtt;
          }
          pending_.erase(pending);
        }
        continue;
      }
      if (kind == 0) {
        if (ready_.size() < kWindow) ready_.emplace_back(payload.begin(), payload.end());
        continue;
      }
      // Reject packets beyond the receive window without ACK, so resends can fill the gap.
      const int32_t delta = int32_t(seq - next_receive_);
      if (delta >= int32_t(kWindow)) continue;
      if (delta >= 0) ordered_.try_emplace(seq, payload.begin(), payload.end());
      std::array<uint8_t, kWireHeader> ack{};
      Put32(ack.data(), kWireMagic); Put32(ack.data() + 4, seq); ack[10] = 2;
      Transmit(ack);
      DrainOrdered();
    }
    if (!has_peer_) return;
    for (auto& [seq, p] : pending_) {
      if (p.last >= 0 && now - p.last < .15) continue;
      if (p.attempts++) ++stats_.resent;
      Transmit(p.wire); p.last = now;
    }
    stats_.loss = stats_.sent ? double(stats_.resent) / double(stats_.sent) : 0;
  }
  TransportState State() const override { return state_; }
  TransportStats Stats() const override { return stats_; }
  void Close() override {
    if (socket_ != kInvalid) { CloseSocket(socket_); socket_ = kInvalid; }
    state_ = TransportState::Closed;
  }
 private:
  struct Pending { std::vector<uint8_t> wire; double last; uint32_t attempts; };
  void DrainOrdered() {
    while (ready_.size() < kWindow) {
      auto it = ordered_.find(next_receive_);
      if (it == ordered_.end()) break;
      ready_.push_back(std::move(it->second)); ordered_.erase(it); ++next_receive_;
    }
  }
  void Transmit(std::span<const uint8_t> wire) {
    const int n = int(sendto(socket_, reinterpret_cast<const char*>(wire.data()), int(wire.size()), 0,
        reinterpret_cast<const sockaddr*>(&peer_), sizeof(peer_)));
    if (n == int(wire.size())) ++stats_.sent;
  }
  Socket socket_ = kInvalid;
  sockaddr_in peer_{};
  bool has_peer_ = false;
#ifdef _WIN32
  bool wsa_ = false;
#endif
  TransportState state_ = TransportState::Failed;
  TransportStats stats_;
  uint32_t next_send_ = 1, next_receive_ = 1;
  std::map<uint32_t, Pending> pending_;
  std::map<uint32_t, std::vector<uint8_t>> ordered_;
  std::deque<std::vector<uint8_t>> ready_;
};
}  // namespace
std::unique_ptr<ICoopTransport> CreateDevUdp(bool host, uint16_t port) { return std::make_unique<DevUdp>(host, port); }
}  // namespace kk::coop
#endif
