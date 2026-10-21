#include "coop_transport.h"
#include "coop_direct_udp.h"
#if defined(KK_DEV_TOOLS)
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <rex/logging.h>
namespace kk::coop {
void ConfigureDevUdpSimulation(DirectUdpOptions& options) {
  // One-way outgoing delay/jitter in ms and datagram loss in percent.
  // Each peer has a different deterministic sequence, including lost ACKs.
  if (const char* env=std::getenv("KK_DEV_UDP_SIM")) {
    UdpSimulation sim;
    const bool parsed=std::sscanf(env,"%lf,%lf,%lf",&sim.delay_ms,&sim.jitter_ms,&sim.loss_percent)==3 ||
        std::sscanf(env,"%lf:%lf:%lf",&sim.delay_ms,&sim.jitter_ms,&sim.loss_percent)==3;
    if (parsed &&
        std::isfinite(sim.delay_ms) && std::isfinite(sim.jitter_ms) && std::isfinite(sim.loss_percent) &&
        sim.delay_ms>=0 && sim.delay_ms<=1000 && sim.jitter_ms>=0 && sim.jitter_ms<=500 &&
        sim.loss_percent>=0 && sim.loss_percent<=50) {
      sim.seed=options.host ? 68 : 69; options.simulation=sim;
      REXLOG_INFO("KK dev UDP simulation: role={} one-way-ms={} jitter-ms={} loss-percent={} seed={}",
          options.host ? "host" : "guest", sim.delay_ms, sim.jitter_ms, sim.loss_percent, sim.seed);
    }
  }
}
std::unique_ptr<ICoopTransport> CreateDevUdp(bool host, uint16_t port) {
  DirectUdpOptions options{host,"127.0.0.1",port,"LDEV23"};
  ConfigureDevUdpSimulation(options);
  return CreateDirectUdp(options);
}
}
#endif
