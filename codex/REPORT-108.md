# Task 108: synthetic shader GPU differential failure

15 October 2026. Worktree `F:/KK-native-renderer/wt/beta6-int`, branch `beta6-int`. Started at `63c0ac2`, which differs from the task's `4d599bc` only by REPORT-105. Implementation and final validation are complete. **All 58 required native/resources/shader CTests pass; including development tests, 65/65 pass.** No push, merge, release, package or game launch.

Read DECISIONS.md first, then TASK-108, GAME-RULES and reports 095, 103 part 2 and 105. No applicable AGENTS.md or standing-decision conflict was found. Work is confined to the shader translator, its tests, private task-108 evidence and this report. The fx-beta6 and opt-beta6 worktrees, shared SDK, player files and graphics/gameplay defaults were not edited.

## Classification of the 279 retained lines

The 279 mismatch/result lines comprise **273 printed component mismatches and six result lines**. The test prints only the first four mismatches per shader in the retained baseline; these are not 279 failing shaders or the complete 2,319 component failures.

| Instruction/input pattern | Printed mismatch lines | Root cause |
|---|---:|---|
| Fetched subnormal used by a predicate, comparison or following data/control flow | 25 | Reference model treated an input that the GPU may flush as exact |
| 2D linear filtering | 24 | Missing independent combined-weight rounding in the reference |
| 3D linear filtering | 50 | Same filtering model defect |
| Cube linear filtering | 122 | Same filtering model defect |
| Filtering inside generated random programs | 47 | Same filtering model defect |
| Filtering inside fuzz program 49 | 1 | Same filtering model defect |
| `vs raw 20261070`: ordered GE with an absolute NaN operand | 4 | Real translated-shader semantic defect under Vulkan's default floating-point contract |
| Total | **273** | Plus six result lines = **279** |

The original 1,757 shaders, seeds and three rounds are retained. Sequential controls with the original translator isolate the causes:

| Reference model | Component mismatches | Shaders with mismatches |
|---|---:|---:|
| Retained Beta 5 / task-105 baseline | 2,319 | 84 |
| Correct subnormal uncertainty only | 940 | 77 |
| Correct subnormals and filter weights | 256 | 1 |
| Final translator and reference | **0** | **0** |

Thus 1,379 original failures are the subnormal reference defect, 684 are the filtering reference defect, and 256 are the NaN GE translation defect. `classification.json` and `classified-baseline-lines.csv` retain the complete printed-line mapping; untruncated diagnostic runs retain every remaining mismatch.

## Reference fixes and controls

Subnormal loads retain their reference value but carry an error interval that includes zero. A later comparison whose outcome can differ becomes unknown; a control decision becomes fragile using the test's existing mechanism. This fixes the previously identified fetched-subnormal case without changing shader predicates to integer nonzero tests. Added fetch regressions for both signs, the smallest/largest subnormal, zero and the smallest normal.

For filtering, a separate windowless compute probe samples the same texture directly, without translated instructions. It reproduces the reference `0.73156431783` versus GPU `0.7369384765625` discrepancy. Four basis channels reveal actual corner weights `(41,87,40,88)/256`, rather than exact bilinear weights approximately `(40.70,87.30,40.70,87.30)/256`. Coordinate uncertainty alone misses that redistribution, especially when coordinate derivatives cancel.

The reference now reads the device's `subTexelPrecisionBits` and additionally bounds independent combined-weight rounding. For weight quantum q, normalized weight errors delta and any anchor m, `abs(sum(delta_i * texel_i)) <= q * sum(abs(texel_i - m))`; using the median minimizes this bound and keeps a constant footprint exact. Both sides of footprint transitions are considered. The global comparison function and its tolerances are unchanged. The new regression accepts the measured hardware result while rejecting a value displaced by 1/16 and a fetch displaced by a quarter texel; a constant channel remains tight. Point filtering is not given a blanket linear-filter allowance.

Linked the new CPU checks against the retained old interpreter: **five assertions fail**. They pass with the corrected interpreter. These are measured reference-model corrections, not a generic threshold increase used to conceal the NaN difference.

## Translator fix in both shader backends

The remaining program computes a NaN from a negative logarithm and uses `abs(NaN) >= 0` in `SETP_GE_PUSH`. Vulkan permits optimizations assuming no NaNs without `SignedZeroInfNanPreserve`; DXC `-Gis` does not enable that optional execution mode. On this device the comparison becomes true, changing finite exports and predicate-controlled writes. This explains the exact zero/one and `-FLT_MAX` differences that filtering bounds cannot address. See the official [Vulkan floating-point rules](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html) and [SPIR-V float controls](https://github.khronos.org/SPIRV-Registry/extensions/KHR/SPV_KHR_float_controls.html).

Added scalar/vector `kk_Ge` helpers that reject NaNs using integer bit classification before the floating-point GE operation. Routed ordered-GE compare, conditional, predicate, kill and max paths through them. The same HLSL is compiled for **SPIR-V and DXIL**; no optional float-control capability or stronger GPU requirement is introduced. Updated the compiler comment so it no longer implies that `-Gis` supplies this Vulkan guarantee.

Added four focused generated regressions: vertex and pixel, container and bare microcode. They test finite outputs from vector/scalar GE, GE push, conditional selection and predicated writes after a negative logarithm. They are appended separately so the original 1,757 GPU programs keep their seeds. The compilation corpus validates both SPIR-V and DXIL for the new cases.

The original translator produces **2,560 mismatches in all four focused GPU regressions**; the fixed translator produces **zero**, with all 768 invocations and 18,432 components compared and no unknown/fragile exclusions. The predicate-only HLSL diagnostic also reduces the original failing shader to zero mismatches, with all 192 invocations and 13,056 components compared. A separate windowless DXIL execution check passes **96/96 comparisons** across 24 pairs containing NaNs, infinities, signed zero and finite values.

## Actual game shaders and pack compatibility

The read-only census parses/translates **4,797 unique translation inputs**, including all **three built-in XEX containers**, with zero parse/translation failures. Both task-105 API packs have **17,924 indexed entries**, with zero missing original-container identities. No game-derived data is committed.

The census finds **zero `SETP_GE_PUSH` game shaders**, and therefore zero instances of the exact failing absolute-NaN predicate pattern. It finds 26 inputs with vector GE and 10 with conditional GE; these now use the guarded implementation too. Static opcode coverage does not prove that arbitrary runtime inputs can never contain NaNs. This establishes a genuine synthetic semantic defect with no confirmed gameplay manifestation, not a visible game regression or an exhaustive runtime-data proof. No game launch was needed; both allowed slots remain unused.

Advanced `kTranslatorVersion` from 4 to **5**, as required by its source contract when generated code changes. The version and embedded common HLSL change `translatorHash()`, which is part of pack compatibility. **Both API packs become stale and players will rebuild them after integration.** No player's cache was deleted or changed. No test build was handed over, so the packaging/owner-cache cleanup checklist was not triggered.

## Validation and evidence

| Check | Result |
|---|---|
| Standalone shaders build | PASS |
| Standalone native build | PASS |
| Native CTest | **33/33 PASS**, 123.12 s |
| Resources CTest | 2/2 PASS |
| Shader CPU CTest | **22/22 PASS**, 249.60 s |
| Shader GPU differential CTest | **1/1 PASS**, 223.30 s |
| Development CTest | 7/7 PASS |
| Required native/resources/shaders total | **58/58 PASS** |
| Including the existing development gate | **65/65 PASS** |
| git diff --check | PASS |

The final GPU run executes **1,761 shaders** (the 1,757 originals plus four regressions), with zero translation/compile failures and zero mismatches. It reports 337,856 invocations: 310,214 compared, 27,642 fragile, and the same four nonterminating rounds skipped as before. Of 7,499,510 counted components, 348,958 are unknown; **7,150,552 numeric component comparisons** remain. No shader name, random seed, round count or reported failure was disabled to obtain a passing gate.

Evidence root: `F:/KK-native-renderer/wt/beta6-int/out/task108/`. Key files: `diff-denorm.log`, `diff-weights.log`, `sample-first.log`, `interpreter-old.log`, `interpreter-fixed.log`, `classification.json`, `classified-baseline-lines.csv`, `census-full.log`, `shaders-fixed-cpu-ctest.log`, `resources-ctest.log`, `dev-ctest.log`, final GPU/native logs and `verify-results.json`. Private build, census and GPU helpers remain there for reproduction. GPU helpers respect the shared lock, user/game checks and other tasks' priority reservations. Only task-owned lock acquisitions are released. One attempted diagnostic relink raced its own running GPU test and failed with permission denied; it was rebuilt successfully after that test exited. The failed build log is retained separately.

All GPU work is windowless and performed on the RTX 4070 Ti. Khronos validation is unavailable, so this is not a Khronos-validation-clean claim. This does not certify other GPUs/drivers or gameplay performance. Zero game launches, no focus operations or desktop screenshots, no user game/save/cache access, no downloads/installations and no Linux work. The other worktrees and their processes/reservations were left alone.

Local implementation commit: `080198f` (Fix ordered shader comparisons and GPU differential reference bounds). Both implementation/report commits carry the required Claude co-author trailer. Stop for Claude's verification.
