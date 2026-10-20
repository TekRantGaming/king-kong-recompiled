#pragma once
#include "coop_transport.h"
#include <string>
#include <string_view>

namespace kk::coop {
struct DirectUdpOptions {
  bool host = false;
  // Numeric IPv4/IPv6. Host: "::" is dual stack, "127.0.0.1"/"::1" are private tests.
  std::string address = "127.0.0.1";
  uint16_t port = 27080;
  std::string join_code;
};
bool ValidJoinCode(std::string_view code);
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
  virtual bool SetReady(bool ready) = 0;
  virtual bool PeerReady() const = 0;
  virtual bool CodeRejected() const = 0;
  virtual uint16_t LocalPort() const = 0;
};
std::unique_ptr<DirectUdpTransport> CreateDirectUdp(DirectUdpOptions options);
}  // namespace kk::coop
