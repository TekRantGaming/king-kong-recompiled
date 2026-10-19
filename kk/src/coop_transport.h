#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace kk::coop {
enum class TransportState { Ready, Failed, Closed };
struct TransportStats {
  double ping_ms = 0, loss = 0;  // loss: resend fraction, an estimate on the dev UDP link
  uint64_t sent = 0, received = 0, malformed = 0, resent = 0;
};
// Owned and called exclusively by the session's network thread. No guest addresses or UI objects.
// Reliable messages must arrive once, in order. Receive drains at most one bounded packet.
class ICoopTransport {
 public:
  virtual ~ICoopTransport() = default;
  virtual bool Send(std::span<const uint8_t> bytes, bool reliable) = 0;
  virtual bool Receive(std::vector<uint8_t>& bytes) = 0;
  virtual void Poll(double now) = 0;
  virtual TransportState State() const = 0;
  virtual TransportStats Stats() const = 0;
  virtual void Close() = 0;
};
#if defined(KK_DEV_TOOLS)
// Host binds the requested loopback port; join binds an ephemeral loopback port.
std::unique_ptr<ICoopTransport> CreateDevUdp(bool host, uint16_t port);
#endif
}  // namespace kk::coop
