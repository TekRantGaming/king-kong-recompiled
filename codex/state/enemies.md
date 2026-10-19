# Shared enemies: C18 source handoff, 19 October 2026

Result: host-authoritative step 1 source exists for Raptor/Worm/Scorpion. Runtime acceptance is NOT VERIFIED.
Code commit a26a02d, branch coop-enemies, F:\kk-native-coop\wt\coop-enemies. Report: ../reports/REPORT-C18.md.
Read RULES.md and DECISIONS-COOP.md first. Claude owns all compilation, unit runs and game/launcher runs.
VERIFIED here means source inspection and static delimiter/preprocessor/hook/diff checks only. No compiler/tests were run.

## Identity and lifecycle

- Pure types/policies: kk/src/coop_protocol.h and coop_enemies.h. Guest adapter: coop_enemies_game.cpp.
- EnemyId: source u64, sequence u32, family u16. Network scope: session epoch plus EnemyWorld(level,start,visit).
- Source for an authored actor: FNV-1a over explicit little-endian numeric bytes of family and XYZ rounded to 1/64 unit.
  Neither a heap address nor an active-table index enters that hash. Signed zero has the same representation.
- Initial census occurs before the first AI pass after a Connected load, not whenever a moving actor is first noticed.
  Capture uses full world registry [world+40,world+28); sample/membership uses scheduled registry [world+304,world+292).
- Duplicate same-family spawn signatures or hash collisions refuse every local participant. No nearest-position fallback.
  Binding validates exact actor/AI/vars/matrix and unique active membership, rejecting hero or mutable-component aliases.
- Spawner key uses marker 0x53504157 plus initial XYZ. Its loaded template selector is vars+1592 or vars+1776.
  Spawner_generate 826F8BC8 calls 82715AC8(source,position), return LR826F9240; only successful returns count.
  source signature combines spawner key and selector, then sequence distinguishes subsequent copies/pool reuse.
  New copies wait for scheduled registration/init before sampling. No duplicate or spawn routine is invoked by replication.
- Old IDs retain tombstones until unload. Temporary AI deactivation does not mean removal; full registration or changed
  components retire the binding. Retired bindings stop participating in alias checks. Capacity is 512 IDs per load.
- GUESS: initial positions/template choices/spawner order agree across compatible processes. Divergent waves, ambiguous
  spawn points, unsupported creation routes and missing guest copies remain unmatched and are logged/left alone.
- Both loads must happen while Connected. Visit counts are per level/start, excluding menu loads. Matching counts are
  required; asymmetric missed loads need a new coordinated session. New Steam-session reuse without fresh processes and
  checkpoint/load barriers are not implemented. Loss releases holds; retaining IDs permits revalidation if the world survived.

## Classification and engine evidence

- Names are verified against read-only F:\KK-native-renderer\analysis\hfr\names.json. census-other is address evidence,
  not a universal creature classifier. Explicit persistent tagoff/init/reflex allowlist only:
  Raptor 8254BCB0/825B9020/82572E40; Worm 825088A0/82516820/82516508; Scorpion 825F2380/826055F0/825FE790.
- Jack, H humans/borrowed partner, KAnn, props, ambient creatures, unknowns, Spider/Scolo/RK and Kong families are excluded.
- Raptor life float vars+20: fix_raptor_path.cpp:104, dev_teleport.cpp; MORT writes it at G/.82:20510.
  Worm health float +5392: damage subtracts at G/.110:19198-19225. Scorpion +5792: G/.79:22957-22985.
- Guest zeroes life and releases dispatch holds for authoritative Died/Removed. Raptor uses the established zero-life path.
  Worm/Scorpion additionally use native track setter 826FA3B0(2, local death block), once per generation, preserving
  scratch/PPC context and current/saved AI/object globals BF64/BF68/BF6C/BF70. No wire function is called.
- Worm PAF selects *82CC9318 at G/.74:19810-19845; Scorpion PAF selects *82CC8C5C at .44:22915-22950.
  Each block must have expected MORT function +20 (82534858/82619FD8). Mismatch logs/refuses that engine call,
  retaining zero-life fallback. This is a guarded death-state transition, not a complete damage stimulus or kill API.
- G = F:\kk-native-coop\kk-recomp\kk\generated\default\king_kong_recomp.N.cpp. Evidence:
  .130:35635-35700 serialized matrix copies; .46:33964-34051 full registration; .32:28364-28618 scheduled table;
  .21:26612 Spawner_init; .92:26955 Spawner_loop; .84:27324-27426 selector/spawn ABI; .6:27634-27721 track setter.

## Protocol, hooks and thread ownership

- Protocol version 3. EnemySnapshot: world, host sample time, bounded count, ID, XYZ/yaw/life/state-function/action/item.
  Each state is 41 bytes; metadata 22 and header 24. At most 20 states = 866 bytes, below the 1024-byte cap.
- 12.5 Hz unreliable snapshots; all census batches are emitted. Reliable Spawned/Died/Removed retry under backpressure,
  including without a fresh frame. Terminal records cannot resurrect via delayed snapshots or spawn events.
- Newest sequence is tracked per entity; world transitions also check visit/global sequence. All decoder paths validate
  exact lengths/counts, families/IDs, finite/ranged floats/time and duplicates before committing output.
- CoopSession owns Channel on its worker. Both dev UDP and Steam exchange mutex-protected Frame value copies.
  Every guest-memory read/write and engine call stays on the game thread at the AI pass. No guest pointer enters a worker.
- Shared 8271D498 hook retains DevSampleTick/Puppet ordering; shared 8271CCB8 hook preserves PuppetHold and original fallback.
  Guest holds only exact selected actor+AI scheduled dispatches with LR8271D304, rechecking identity/connection/environment.
- Interpolation: latest arrival anchors remote time, 100 ms delay, no extrapolation, live expiry after 500 ms.
  Native 827117E0,82711278,82401AA8 set translation/facing/controller; legal 82850100 requests action changes with restart=0.
  Animation item bounds are checked by the engine (.103:34117-34120); no arbitrary state function is dispatched from a packet.
- Co-op disabled/Connecting/unmatched paths do not drive or hold enemies. Dev status/logs show sent/matched/unmatched/driven.

## Claude validation and next step

- Build king_kong, kk-enemies-tests, kk-coop-tests, kk-steam-tests; run coop-enemies/coop-net-core/coop-steam-services.
  Build DEV_TOOLS OFF as well. Pure test sources cover identity/matching fakes, every truncation, bounds, lifecycle,
  interpolation/expiry, pool generations, authority, reliable retry, capacity and Steam channel validation. All UNRUN.
- REPORT-C18 has the two-window entry-7/start-3 command, private per-role shots and required visual/lifecycle/regression checks.
  No universal death, missing-actor creation, targeting, mission/script authority or guest-shot damage is implemented.
- Step 2: add shot intents and a bounded game-thread queue, validate world/EnemyId/generation and weapon/ammo/projectiles,
  resolve hit/damage on host through normal type-4 events, then replicate accepted outcomes. C01 documents unresolved aim ABI.
