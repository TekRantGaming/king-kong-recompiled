# C18: shared enemy identity, movement and life/death

Result: step 1 is implemented in source for raptors, worms and scorpions; Claude must establish runtime acceptance.
Code commit: `a26a02d` on `coop-enemies`, F:\kk-native-coop\wt\coop-enemies. This report/state follow in a documentation commit.
Files: new coop_enemies.h, coop_enemies_game.{h,cpp}, coop_enemies_tests.cpp; protocol/session, both workers,
Steam transport, shared AI hooks, load callbacks, dev status and CMake integration. Details: ../state/enemies.md.
VERIFIED: generated/source inspection, balanced delimiters/preprocessor conditionals, unique shared hook owners, git diff --check.
UNRUN: every compiler, build, unit binary, game and launcher. New CTest: coop-enemies (kk-enemies-tests).

EnemyId = (64-bit source signature, u32 successful-spawn sequence, u16 family), scoped to session + level/start/visit.
Authored source = endian-independent FNV-1a of family and first post-load XYZ quantized to 1/64 guest unit; sequence=0.
The full world registry (+40/+28) discovers dormant actors; the scheduled table (+304/+292) supplies live samples.
No heap addresses or mutable enumeration indices cross the link. Duplicate local signatures and mutable aliases refuse matching.
Evidence: G/king_kong_recomp.130.cpp:35635-35700 loads/copies transforms; .46:33964-34051 registers world objects;
.32:28364-28618 walks scheduled objects. G = F:\kk-native-coop\kk-recomp\kk\generated\default.
Waves use the unique spawner signature + template selector (vars+1592 or +1776) + successful 82715AC8 creation number.
Evidence: Spawner_init 826EB8C0 (.21:26612), loop 826E1EE8 (.92:26955), generate 826F8BC8 (.84:27324-27426).
The observed call returns at 826F9240. Pool reuse gets a new generation; delayed registration waits, removed IDs retain tombstones.
GUESS requiring two-machine proof: initial spawn points and spawner order agree in compatible loads; divergent waves stay unmatched.

Allowlist: PNJ_Raptor tagoff/init/reflex 8254BCB0/825B9020/82572E40; Worm 825088A0/82516820/82516508;
Scorpion 825F2380/826055F0/825FE790. Names verified in read-only analysis/hfr/names.json; census-other prints track addresses.
Jack, borrowed H partner, H allies, KAnn, props, ambient creatures and unresearched families (including spiders/Scolo/RK) are excluded.
Raptor life vars+20: fix_raptor_path.cpp:104 and G/.82:20510. Worm +5392: .110:19198-19225; Scorpion +5792: .79:22957-22985.
Guest death releases AI after zero life; Worm/Scorpion additionally request local track 2 via engine 826FA3B0, once per ID.
Loaded death-block globals 82CC9318/82CC8C5C must resolve to 82534858/82619FD8 at +20, otherwise log/refuse that transition.
Evidence: Worm PAF .74:19810-19845, Scorpion PAF .44:22915-22950; setter .6:27634-27721. Caller context/globals are restored.

Protocol v3: 12.5 Hz unreliable batches, at most 20 enemies / 866 bytes; reliable Spawned/Died/Removed with backpressure retry.
Workers exchange value copies only. Host AI remains normal. Guest holds only exact matched actor+AI scheduled dispatches at LR8271D304,
uses 100 ms arrival-anchored interpolation, expires live samples after 500 ms and uses engine position/facing/controller/action setters.
Limits: 512 IDs per load; no missing-actor construction, universal kill API, script authority, target selection or guest damage forwarding.
Both peers must load while Connected with matching per-level/start visit counts. An asymmetric missed initial load needs a fresh session;
late connection does not invent identities from moved actors. Checkpoint barriers and recovery after local death still need mission sync.

Claude: integrate/review the code commit, then in F:\kk-native-coop\kk-recomp\kk configure kk-dev-vulkan and build king_kong,
kk-enemies-tests, kk-coop-tests, kk-steam-tests; run CTest regex '^(coop-enemies|coop-net-core|coop-steam-services)$'. Build kk-beta with DEV_TOOLS OFF.
Two-window choice: entry 7 (Hayes, level 303), KK_DEV_START_POINT=3. Read-only codex/state/hayes-dino.md:41-48 documents its two-raptor intro at (128,92).
UNRUN command, after Claude's usual game-slot checks; each -Env/-JoinEnv receives exactly one string:
```powershell
$shots = 'F:\kk-native-coop\runs\c18-shots-' + (Get-Date -Format yyyyMMdd-HHmmss)
New-Item -ItemType Directory -Path "$shots\host","$shots\join" -Force | Out-Null
& F:\kk-native-coop\tools\coop-run.ps1 -Name c18-enemies -Entry 7 -Seconds 65 -Two -Snap '15,25,35,45,55' -Env "KK_DEV_START_POINT=3|KK_DEV_AUTOSKIP=1|KK_DEV_SCRIPT=0:SKIP/10|KK_DEV_WANDER=0|KK_DEV_SHOTS=15,25,35,45,55|KK_DEV_SHOTS_FROM=menu|KK_DEV_SHOTS_DIR=$shots\host" -JoinEnv "KK_DEV_SHOTS_DIR=$shots\join|KK_DEV_SCRIPT=0:SKIP/10,20:LSLEFT+LSHALF/0.4"
```
Require matching identity/visit logs, increasing host sent and guest matched/driven; compare enemy movement/facing/action in both captures.
Kill with normal host shots: guest must reach death/fade once. Exercise wave reuse/removal, unmatched leave-alone, reload, expiry/loss release,
co-op disabled/Connecting, puppet regression, 30/60/120 FPS Vulkan and D3D12 smoke; inspect any death-transition refusal. No runtime result claimed.
Step 2 needs sequenced shot intent (world/EnemyId, origin/direction, weapon/timing), host game-thread lookup/raycast and normal type-4 damage,
weapon/ammo/projectile validation and authoritative outcomes. C01's cached hero hit resolver is not yet a remote-shot ABI.
All writes stayed in this worktree. Protected renderer untouched; no SDK files, public notes, pushes, merges, releases or posts.
