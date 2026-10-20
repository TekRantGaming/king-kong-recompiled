#include "coop_transport.h"
#include "coop_direct_udp.h"
#if defined(KK_DEV_TOOLS)
namespace kk::coop {
std::unique_ptr<ICoopTransport> CreateDevUdp(bool host, uint16_t port) {
  return CreateDirectUdp({host,"127.0.0.1",port,"LDEV23"});
}
}
#endif
