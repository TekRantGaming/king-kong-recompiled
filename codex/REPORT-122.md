# Task 122: draw distance / LOD and SMAA

15 October 2026. Worktree `F:/KK-native-renderer/wt/gfx-lod`, branch `gfx-lod`, baseline `bab085c` (`beta6-tb1`). Read `F:/KK-native-renderer/DECISIONS.md` first, then TASK-122 and GAME-RULES. No applicable AGENTS.md was found.

**Item 6: investigation completed to the task's gameplay-coupling stop condition; no setting added. Item 7: not implemented because the MIT-source requirement conflicts with the standing download restriction.** No game launches (0/6), captures, performance measurements, gameplay changes, renderer changes, pushes, merges, releases or packages. Task 121's sampler/effect-buffer files were not edited.

## 6. Draw distance / LOD

The game has a projected-size importance byte shared by rendering and AI. Multiplying that value or extending the visibility path is not an isolated graphics change. The task explicitly says: "if LOD and gameplay are coupled, stop at a written finding." Stopped here; no speculative global override, per-level patch, or Original / Far / Farthest control was introduced.

### Traced paths

All addresses below are guest addresses. Generated C++ and disassembly were inspected read-only and remain outside the commit.

| Routine | Finding |
|---|---|
| `sub_828732E8` | Per-view rendering driver. Calls `sub_82872DB8`, constructs view/culling state, and calls `sub_82873130`. |
| `sub_82873130` | Iterates objects, calls `sub_82867B10(object, GDI +208)` for visibility, and writes zero to **object +28** on its rejected-object branch. For accepted objects it calls `sub_82872A28` and stores the returned byte at **object +28**. |
| `sub_82872A28` | Computes a projected-size quantity from the object's bound, camera distance, and view-dependent multipliers; converts it to a byte, capped at 255 with a minimum nonzero result of 1. Object +29, when nonzero, supplies another multiplier divided by 255. This is not a fixed world-distance switch. |
| `sub_827D2080` | Reads the current object's byte at +28 through **GDI +156**. Reads a child count at resource +8 and byte thresholds starting at **resource +10**; walks those thresholds and selects a child pointer starting at **resource +16**. This is a concrete resource LOD selection path. |
| `sub_827D2250` | Resource loader for that path: caps the child count at **six**, reads six threshold bytes and loads the child references. Thus these thresholds are authored resource data, rather than one named global distance in the INI. |
| `sub_827D2190`, `sub_827D2388`, `sub_82866640` | Allocate/initialize the resource and install the `sub_827D2080` selector in its dispatch table. The selector is dispatched indirectly, explaining the absence of a direct call. |
| `sub_82876FA0`, `sub_82855BB8`, `sub_82855908` | Set **GDI +156** to the current object before indirect rendering/resource callbacks, connecting the selector's byte read to the object written above. |

The projected-size calculation uses a radius or an AABB diagonal divided by distance, with multipliers loaded through `view +168` at offsets +228 and +112. These are candidate inputs to investigate, **not established graphics-only scale controls**. No new default or numeric distance factor was chosen.

### Gameplay coupling, with concrete consumers

`sub_8271D498`, the AI dispatcher named in DECISIONS.md, reads **object +28**. When the object's descriptor flags at `*(object +24) +46` include bit `0x4`, a zero byte skips that object before the dispatcher reaches `sub_8271D1D8`. `sub_8271CB00` has the same descriptor-flag/zero-byte gate before another update path.

There is also coupling beyond the zero/nonzero visibility decision: `sub_8271B838` compares **object +28** against **10, 25 and 100**, sets bits in the associated object's +166 state using its +167 mask, and can call `sub_8271B418` when that state changes. It is called by `sub_8271C1C8` and `sub_8271C590`. Therefore even a multiplier that preserves nonzero values can change these engine-side decisions. The exact semantic meaning of every state bit has not been established; this is sufficient to reject a claim of gameplay independence, not a claim that every possible rendering-only LOD modification is impossible.

No AI step was throttled, bypassed, or otherwise changed.

### INI, fog and streaming limits

The supplied `kk/assets/Jade_xe.ini` contains settings for binarized data, BIG file paths, buffering and parallel GDK; it contains no LOD scale, view-distance or fog-distance option. It was not edited.

`sub_82872DB8` copies the active view into GDI, calls the projection callback (the existing FOV investigation identifies `sub_8278AB00`) and builds the frustum with `sub_827E5BB8`. The current FOV hook restores the view after that call. Neither this path nor the INI establishes an independent draw-distance scale.

Distance fog is the separate AfterEffects pass `sub_827648A8`, already wrapped by `kk/src/fog.cpp`. Disabling it suppresses the pass; it does not make that hook a geometry/streaming control. The existing launcher warning also documents that fog hides empty or unfinished area edges. **A common fog/LOD/streaming distance variable was not proven.** Per-level streaming limits and world-space distances were not decoded after the coupling stop condition was reached. They must not be inferred from the threshold bytes above.

Outdoor-vista, V-Rex canyon and Skull Island coast before/after captures and CPU/GPU costs are **not available**: there is no changed draw-distance implementation to compare. No additional game runs were spent on a rejected global override.

## 7. SMAA 1x: standing-decision conflict

The task requests the **MIT reference implementation**. DECISIONS.md says: **"Downloads come from official project release pages only."**

Checked the [official SMAA project/download page](https://iryoku.com/smaa/), [official v2.8 release/tag page](https://github.com/iryoku/smaa/releases/tag/v2.8), and upstream release/tag metadata. The newest listed tag is v2.8 (`c9ff6c0`, 23 August 2013); no later release was found. Downloaded that official tagged release archive only:

- `F:/KK-native-renderer/downloads/smaa-v2.8-task122.zip`
- SHA-256: `f6192b0759f3a9f52769f328832f61870849d2ba19dbae7718e0d76d3411befc`
- Extracted to `F:/KK-native-renderer/downloads/smaa-v2.8-task122/smaa-2.8/`.

Its `LICENSE.txt`, `SMAA.h`, and lookup-texture headers carry the older BSD-style conditions, including a binary-distribution credit requirement. They are **not the requested MIT version**. The [current upstream licence](https://github.com/iryoku/smaa/blob/master/LICENSE.txt) and README identify MIT source on the later development branch; that is not the source in the permitted release archive. No development-branch archive/raw-source download was made, no source was silently relicensed, and the older release was not substituted for the requested MIT implementation.

Per the user's instruction to skip only conflicting work, stopped item 7 and completed the independent LOD investigation and validation. Integration needs a standing-decision exception for a pinned official upstream MIT source snapshot, or a qualifying official MIT release.

Existing integration points were inspected: `GameRenderer::Present` currently applies FXAA after the gamma-ramp copy; `swap_post_effect` is parsed by `plugin_settings.cpp` and exposed by the launcher/F8 panel; initialization through `PipelinePrewarm` creates a `GameRenderer`. No SMAA UI choice, shaders, lookup textures, passes, prewarm entries or pack changes were added. Both API support, native-resolution behavior, comparison crops versus FXAA/off and SMAA cost remain **unimplemented/unmeasured**. This report does not claim that baseline tests validate SMAA.

## Validation

**All 70 baseline CTests pass.** These validate the unchanged worktree code, not either requested feature. Built the native renderer/plugin and its tests, standalone resources and shaders, and development test targets in this worktree. No `king_kong.exe` build or test package is being handed over.

| Suite | Result | Time |
|---|---|---|
| Native renderer, including both API requirements/upscaler tests and SPIR-V validation | 36/36 PASS | 268.74 s |
| Resources | 2/2 PASS | 11.74 s |
| Development/settings/launcher/panel/pacing/lock tests | 9/9 PASS | 2.02 s |
| Shader CPU, database and interpreter | 22/22 PASS | 314.22 s |
| Shader GPU differential | 1/1 PASS | 174.39 s |
| Total | **70/70 PASS** | |
| `git diff --check` (staged report) | PASS | |

GPU differential: **1,761 shaders**, zero translation/compilation failures, **zero mismatches**. The existing exclusions remain: 27,642 fragile invocations, 348,958 unknown components and four nonterminating rounds. GPU tests used the RTX 4070 Ti offscreen. Khronos validation is not installed; NVRHI validation and the checked-in SPIR-V validation passed. FSR 4 is unavailable on this NVIDIA GPU and the SDK checks exercised its documented fallback. Optional upscaler-cost benchmarks were not enabled; these tests do not supply the missing feature costs.

Initial test attempts omitted the SDK's DLL directory from `PATH`; runtime-linked tests failed to load with `0xc0000135`, and the initial native suite was interrupted. Only task-owned test/CTest PIDs were targeted. Corrected the private test runner's environment, suppressed system error dialogs for subsequent children, and reran the affected suites successfully. Initial logs are retained as `dev-ctest-initial.log` and `native-ctest-initial.log`. Resources had already passed and did not need repeating.

The existing atomic `codex/game-slot.ps1` guard reserved the slot for offscreen native/GPU validation, waited for task 121's first reservation to finish, and released only task 122's reservation. No task-owned test process remains. The other task's later reservation was left untouched.

Private evidence/build root: `F:/KK-native-renderer/wt/gfx-lod/kk/out/task122/`. It contains `asm.json`, `locations.json`, routine `.asm` extracts, `build-tests.cmd`, `run-tests.ps1`, build/configuration logs and CTest logs. Generated code, shader packs, game assets and evidence are ignored/uncommitted. No pictures were produced for `F:/KK-native-renderer/documentary/gfx-lod/`.

Updated only items 6 and 7 in `F:/KK-native-renderer/WORK-LIST.txt`: **partial** (LOD investigation, gameplay coupling, no setting) and **not done** (SMAA MIT-source/download conflict). Preserved the other jobs' entries. The report is the only tracked worktree change; no production code or game-derived data is committed.

No user game/save/cache locations were accessed or modified. No system components were installed, no Linux work was undertaken, and no test build is being handed over, so the packaging/cache-deletion checklist does not apply. No push, merge or release. Stop for Claude's verification.
