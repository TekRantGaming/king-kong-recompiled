#pragma once
#include "coop_transport.h"
#include "steam_lobby.h"
#include <string>
#include <string_view>

namespace kk::coop {
struct DirectUdpOptions {
  bool host = false;
  // Numeric IPv4/IPv6. Host: "::" is dual stack, "127.0.0.1"/"::1" are private tests.
  std::string address = "127.0.0.1";
  uint16_t port = 27080;
  std::string join_code;
#if defined(KK_DEV_TOOLS)
  struct Simulation { double latency_ms=0,jitter_ms=0,loss_percent=0; } simulation;
#endif
};
bool ValidJoinCode(std::string_view code);
std::string NormalizeJoinCode(std::string_view code);
std::string NewJoinCode();
std::string DirectLocalAddresses();
// Clock-injected token bucket. Both work and memory remain bounded under a flood.
class UdpRateLimit {
 public:
  UdpRateLimit(double rate, double burst) : rate_(rate), burst_(burst), tokens_(burst) {}
  bool Take(double now, double count = 1);
 private:
  double rate_, burst_, tokens_, last_ = -1;
};
class DirectUdpTransport : public ICoopTransport {
 public:
  virtual bool SetReady(bool ready, uint64_t revision = 0) = 0;
  virtual bool SetChoice(LobbyChoice choice) = 0;
  virtual LobbyChoice Choice() const = 0;
  virtual bool PeerReady() const = 0;
  virtual bool CodeRejected() const = 0;
  virtual uint16_t LocalPort() const = 0;
};
std::unique_ptr<DirectUdpTransport> CreateDirectUdp(DirectUdpOptions options);
}  // namespace kk::coop
