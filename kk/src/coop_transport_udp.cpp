#include "coop_transport.h"
#include "coop_direct_udp.h"
#if defined(KK_DEV_TOOLS)
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <rex/logging.h>
#include <rex/cvar.h>
REXCVAR_DEFINE_STRING(kk_coop_udp_profile, "", "KK/Debug", "Dev UDP faults: lan, home, bad_wifi, mobile, hell")
    .debug_only().lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(kk_coop_udp_seed, 0, "KK/Debug", "Dev UDP fault seed (0 uses profile default)")
    .debug_only().lifecycle(rex::cvar::Lifecycle::kInitOnly).range(0, 2147483647);
REXCVAR_DEFINE_STRING(kk_coop_udp_outage, "", "KK/Debug", "Dev UDP outage: host|guest|both,at-seconds,duration-seconds")
    .debug_only().lifecycle(rex::cvar::Lifecycle::kInitOnly);
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
  const auto name=REXCVAR_GET(kk_coop_udp_profile);
  if (!name.empty()) {
    UdpSimulation sim;
    if (!UdpProfile(name,sim)) { REXLOG_ERROR("KK dev UDP: unknown profile '{}'",name); return; }
    const auto seed=REXCVAR_GET(kk_coop_udp_seed);
    if (seed>0) sim.seed=uint32_t(seed);
    options.simulation=sim;
  }
  const auto outage=REXCVAR_GET(kk_coop_udp_outage);
  if (!outage.empty()) {
    char role[6]{},tail=0; double at=0,seconds=0;
    auto sim=options.simulation;
    const bool parsed=std::sscanf(outage.c_str(),"%5[^,],%lf,%lf%c",role,&at,&seconds,&tail)==3;
    const std::string_view direction(role);
    if (direction=="both") { sim.disconnect_at=at; sim.disconnect_seconds=seconds; }
    else { sim.one_way_roles=direction=="host" ? 1 : 2; sim.one_way_at=at; sim.one_way_seconds=seconds; }
    if (parsed && (direction=="host" || direction=="guest" || direction=="both") && ValidUdpSimulation(sim)) options.simulation=sim;
    else REXLOG_ERROR("KK dev UDP: invalid outage '{}'",outage);
  }
  if (!name.empty() || !outage.empty()) {
    const auto& s=options.simulation;
    REXLOG_INFO("KK dev UDP chaos: role={} profile={} seed={} delay-ms={} jitter-ms={} loss-percent={} reorder-percent={} duplicate-percent={} burst-enter={} burst-exit={} burst-loss={} bandwidth-bps={} buffer-ms={} one-way={},{},{} disconnect={},{} model=C77-v1",
        options.host ? "host" : "guest",name,s.seed,s.delay_ms,s.jitter_ms,s.loss_percent,
        s.reorder_percent,s.duplicate_percent,s.burst_enter_percent,s.burst_exit_percent,s.burst_loss_percent,
        s.bandwidth_bps,s.buffer_ms,s.one_way_roles,s.one_way_at,s.one_way_seconds,s.disconnect_at,s.disconnect_seconds);
  }
}
std::unique_ptr<ICoopTransport> CreateDevUdp(bool host, uint16_t port) {
  DirectUdpOptions options{host,"127.0.0.1",port,"LDEV23"};
  ConfigureDevUdpSimulation(options);
  return CreateDirectUdp(options);
}
}
#endif
