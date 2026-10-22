# Private co-op fault tests

Fault model version: C77-v1. Source baseline: coop be30d5dd, protocol 17.
Method and profile values adapted from coop-anything revision
b877c9e46afe0f49a30430b17891603670fb61d0. Copyright (c) 2026 cranebeaker and
contributors. The full MIT notice is in LICENSE. coop_mut.h and run-twins.py
are adapted copies; the KK test adapter and guard sites are new.

The outgoing dev UDP model sits below the existing nonce/reliable framing.
ACKs, Hello, lobby control and retries all pass through it. Steam is unchanged.
The beta build has no fault-model decisions or mutation environment reader.
COOP_RUNTIME_MUT is defined only for kk-chaos-tests, never king_kong.

Dev-only arguments (both peers):

```
--kk_coop_udp_profile=bad_wifi --kk_coop_udp_seed=77
--kk_coop_udp_outage=guest,30,4
```

Outage syntax is `host|guest|both,at-seconds,duration-seconds`, relative to
each transport's first Poll. `both` is a full disconnect. The role names mean
that role's outgoing traffic, including ACKs. Supply matching arguments to
both peers to schedule a full outage. One outgoing stream may also be cut in
isolation with SetSimulation in tests. Seed 0 selects the profile's default.
The legacy KK_DEV_UDP_SIM=delay,jitter,loss remains supported; an explicit
named profile takes precedence. Jitter still permits implicit reordering.

| Profile | Seed | Delay/jitter ms | Loss % | Burst enter/exit/loss % | Reorder/duplicate % | Cap bps/queue ms |
|---|---|---|---|---|---|---|
| lan | 1001 | .5/.2 | 0 | 0/50/100 | 0/0 | unlimited |
| home | 1002 | 12/4 | .5 | 0/50/100 | 0/0 | unlimited |
| bad_wifi | 1003 | 35/25 | 2 | 1/25/60 | 2/.5 | unlimited |
| mobile | 1004 | 70/40 | 1 | 1/20/50 | 1/0 | 2000000/300 |
| hell | 1006 | 200/150 | 8 | 3/10/90 | 10/5 | 512000/500 |

Seeded decisions repeat for the same outgoing packet schedule. Real game
scheduling is not bit-exact replay. Host and guest have independent random
streams and independent clocks. Duplicates consume cap bandwidth separately;
the cap never removes serialization delay when reordering skips latency.
Queue memory is bounded to 512 datagrams and the reliable window stays 64.
SimulationStats records model decisions; forwarded/copies counts admissions,
not OS send completion. queue_full is backpressure, not simulated loss.
Dev diagnostics print counters every pose diagnostic interval on the UI thread.

Build only using the authorized kkbuild.bat wrapper. Tests:

```
ctest --test-dir <worktree>/kk/out/build/kk-dev-vulkan -R coop --output-on-failure
C:/Users/TekRa/AppData/Local/Programs/Python/Python312/python.exe -I <worktree>/tools/run-twins.py <worktree>/tools/coop-chaos/twins.json --verbose --json <private-evidence>/twins.json
```

Each named guard baseline must pass. Every twin must exit 1 with its named
FAIL assertion. Timeouts, crashes and loader failures are not catches.
All coop source/header mutation sites are checked against the table, excluding
framework examples in coop_mut.h. No shipping target enables COOP_RUNTIME_MUT.

The profile test runs 40 seeds per profile (200 actual core scenarios) using
real private loopback sockets, real reliable ACK/retry/order/nonces, the protocol,
sessions, save transfer, load barriers, ordered story actions and stale-world
rejection. Separate tests force duplicates/reorder, stale epochs/owners, digest
differences, shot-origin rejection, false quiet repairs, both one-way directions,
5-second reconnection and a 25-second disconnect ending both peers.
Native hooks/AI/rendering/save files are absent from this adapter. It does not
prove native world equality, native repairs or pickup behavior.

Extension points for C71: add object cumulative-damage monotonic and duplicate
replay guards to this executable and twin table only after C71 is integrated.
The baseline has no object codec or personal pickup replication guard; do not
invent a fake policy to claim coverage. Add the pickup rule twin at its real
native decision seam when available. The current nine twins cover real guards.

Game evidence must include source commit, dirty paths and executable SHA-256.
Use only F:/kk-native-coop/tools/coop-run.ps1 to launch. Record all run*.log
rotations and actual mode/counters. A two-window run is separate evidence from
these engine-free tests.
