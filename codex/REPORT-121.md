# Task 121: graphics quick wins 1-5

15 October 2026. Worktree `F:\KK-native-renderer\wt\gfx-quick`, branch `gfx-quick`, base `bab085c`
(beta6-tb1). Source candidate for Claude's verification. No push, merge or release.

Read DECISIONS.md first, then TASK-121, GAME-RULES, formats.md's mip-tail section, upscalers.md and
REPORT-101/108. No standing-decision conflict required skipping a task part. Vulkan remains primary;
no frame generation, production game/AI timing changes or unrelated work were added.

| Item | Status | Default/result |
|---|---|---|
| 1. Temporal material mip bias | **Partial** | Auto uses vendor offsets; supported SDKs captured on both APIs at both outputs. FSR 4 hardware and isolated sampling cost remain unverified. |
| 2. Effect-buffer resolution | **Partial** | Output scaling and Original option implemented; V-Rex and synthetic size/coordinate checks pass. Individual bloom/blur/DOF/glow acceptance and isolated cost remain. |
| 3. Packed mip tails | **Partial** | Default stays **Off**. Vulkan golden result 4/5, D3D12 5/5; no conclusive minified decal/icon proof. |
| 4. FidelityFX CAS | **Done** | Optional Off/Low/Medium/High slider in Graphics and F8, five languages; default **Off**. |
| 5. High/Ultra filtering | **Done** | Both use **16x**. Existing values are retained; explicit High/Ultra selections remain distinguishable. |

Updated only items 1-5 in `F:\KK-native-renderer\WORK-LIST.txt`.

## 1. Material mip bias

Inspection found an existing automatic bias, sampler keying in 1/32 steps, jittered-scene draw guard,
and live mode-change plumbing. The existing formula lacked the extra vendor offset. Automatic bias
now uses the SDK's actual input/output ratio: DLSS and FSR 3.1/4 use `log2(input/output)-1` below
output size; XeSS uses `log2(input/output)`. Native AA keeps the game's bias. Numeric choices remain.
Resolved textures and textures with only one host mip are excluded even when a material draw reads
them. The existing scene/post guard remains; UI/post/shadow/depth samplers do not receive a blanket
global sampler change.

At Quality, the captured input/output pairs are 1707x960 -> 2560x1440 and 2560x1440 -> 3840x2160.
Runtime metadata confirms approximately -1.585 for DLSS/FSR and -0.585 for XeSS on Vulkan and D3D12.
The sampler adds this to the guest bias, quantised to the existing 1/32 grid. Startup output changes
and live upscaler-mode changes rebuild the ratio through the existing setup path; output selection
is still a restart setting. The default post upscale point was tested; the diagnostic Present point
was not separately exercised.

References: [NVIDIA section 3.5](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf),
[AMD](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/#mipmap-biasing),
[Intel](https://www.intel.com/content/www/us/en/developer/articles/guide/xe-super-sampling-developer-guide.html).
The locally installed NVIDIA/Intel SDK guides were checked too. FSR 4 cannot run on this RTX 4070 Ti;
its offset branch is unit-tested, not an FSR 4 gameplay certification.

Pictures: `documentary/gfx-quick/1-mip-bias/`, with `baseline-vs-mip-dlss`,
`fsr-before-vs-mip-fsr` and `xess-before-vs-mip-xess` sheets for every final matrix row below.
Stone/wood detail changes are visible with DLSS. FSR/XeSS differences can be comparable to rain noise;
the images do not establish improved temporal stability. No extra GPU pass is added; isolated
sampling/bandwidth cost was not established. Whole-frame observations are retained below.

Implementation commit: `dbde2c3`.

## 2. Effect buffers

Inspection found that general target scaling and the viewport/scissor, resolve and `texInvScale`
coordinate fixes already existed. Post-phase small targets already followed output resolution;
scene-phase targets followed the temporal input size. The new `native_effect_buffers` option makes
the census's 320x180 and 640x360/480 targets, including matching depth attachments, follow output
scale in either phase. One common scale (the smaller output-axis scale) preserves target aspect.
Known shadow-map pitches are excluded. CPU-uploaded video planes are not resized by this rule.

Graphics and F8 offer Original/Output in all five languages. Default Output preserves the existing
scaled-post behaviour and extends it to the early small targets; Original look selects Original.
Target recreation occurs at the existing frame boundary. Tests cover original/scaled/original
switching and nonuniform output scale, and the existing VPOS/unnormalised-fetch and resolve tests pass.

Pictures: `documentary/gfx-quick/2-effect-buffers/*-baseline-vs-effects.png`.
The checked V-Rex framing remains aligned. This one scene does not identify and sign off every
bloom, blur, DOF and glow shader family. Whole-frame timing is confounded, so there is no reliable
isolated effect-buffer cost claim. Keep item 2 partial for broader visual and performance acceptance.

Implementation commit: `ac53fba`.

## 3. Mip tails

The resource tests, including the existing base-tail layout/conversion checks, pass. No converter
change was made. Both game runs used `native_texture_tail_mips=true`, Original look, 720p, 30 FPS,
no temporal upscaler, and the unchanged five-scene historical V-Rex script/thresholds.

| API | Loading | V-Rex 110 | V-Rex 140 | V-Rex 170 | Pause | Scenes |
|---|---:|---:|---:|---:|---:|---:|
| Vulkan | 0/3 fail | 4/5 pass | 3/5 pass | 5/5 pass | 3/3 pass | **4/5** |
| D3D12 | 3/3 pass | 5/5 pass | 5/5 pass | 5/5 pass | 3/3 pass | **5/5** |

These are frame pass counts using each scene's existing minimum, not every-frame passes. Vulkan's
loading comparison reports worst MAE 0.427 and fails tile/SSIM/bad-pixel checks. There is no matching
same-build Off run at 720p, so this does not attribute the failure to mip tails. No specific minified
tail decal/icon was conclusively isolated. The requested test census did not produce a census file
in these native runs. The evidence does not justify enabling the default, which remains **false**.
There is no isolated GPU cost measurement for this option and no claimed global 360 match.

Reports and pictures:

- [Vulkan golden report](F:/KK-native-renderer/documentary/gfx-quick/3-mip-tails/task121-vulkan-1-tail1-20261015-211819/report/report.html)
- [D3D12 golden report](F:/KK-native-renderer/documentary/gfx-quick/3-mip-tails/task121-d3d12-1-tail1-20261015-211958/report/report.html)

The golden references were read only. No Classic renderer was launched. A 1440p/4K paired tail-only
comparison remains outstanding; the eight-run budget was not exceeded to obtain it.

## 4. CAS

Added a sharpen-only FP32 adaptation of AMD FidelityFX CAS from the locally present official
FidelityFX SDK 1.1.4. The complete MIT notice is in `backend/shaders/cas.hlsli`. Low/Medium/High use
CAS sharpness 0/0.5/1. The four-position slider appears in Graphics and F8, including controller
adjustment, Original-look handling, temporary settings, saving and Graphics-page reset. English,
German, Spanish, French and Italian text passes the text audit.

CAS runs after the game's finished output/gamma ramp and FXAA, or after temporal upscaling and the
game's post chain. It filters linearised sRGB, clamps border reads and preserves alpha. Active FSR
RCAS and spatial FSR/NIS take precedence; CAS is skipped. Off adds no sharpening pass. The extra
scratch image is retained for reuse, approximately 14.1 MiB at 1440p or 31.6 MiB at 4K at four bytes
per pixel, excluding API allocation alignment.

The first regression caught CAS wired into the FXAA sampling helper instead of the final blit.
That was corrected before these game captures. Final native tests check flat black/white/colour,
border reads, real midtone edge enhancement, alpha and FXAA on/off, including reduced descriptor
tables. Both DXIL and SPIR-V programs were regenerated with official local DXC 1.9.

Pictures: `documentary/gfx-quick/4-cas/`, with Low/Medium/High, FXAA/CAS and FSR-RCAS/CAS skip
comparisons for all four final matrix rows. High visibly strengthens fine grain and rain/foliage;
this sample does not justify adding automatic sharpening to every chapter. Default is **Off**.
No preset silently enables CAS. FSR RCAS skip comparisons remain within the moving-scene noise.

Measured final CAS blit/filter pass, median milliseconds on this RTX 4070 Ti:

| API/output | Low | Medium | High | CAS after native FXAA |
|---|---:|---:|---:|---:|
| Vulkan 1440p | 0.145 | 0.140 | 0.132 | 0.064 |
| Vulkan 4K | 0.271 | 0.247 | 0.222 | 0.140 |
| D3D12 1440p | 0.159 | 0.146 | 0.145 | 0.091 |
| D3D12 4K | 0.158 | 0.154 | 0.142 | 0.142 |

These queries exclude scratch allocation and its preceding copy/FXAA work. Clock/load variation
limits comparisons between strength levels. This is not a measured whole-frame overhead budget.

Implementation commit: `c0a98e9`.

## 5. Preset filtering and user choices

High changes from 8x to 16x; Ultra was already 16x. Low/Medium/Steam Deck remain 2x/4x/2x. Loading
an existing configuration does not replace its filtering value. An existing 8x choice remains 8x
and can show Custom; explicitly selecting High or Ultra applies 16x.

This makes High/Ultra's current tables identical. Added the internal saved `kk_graphics_preset`
label so selecting either remains distinguishable. The preference is honoured only while the actual
settings match; an unmarked old 16x Ultra configuration still recognises as Ultra. Tests check both
explicit selections and preservation of an 8x custom value. Graphics reset includes the preference.

Pictures: `documentary/gfx-quick/5-anisotropy/*-baseline-vs-aniso16.png`. These are 8x/16x controls,
not an isolated timing benchmark. Preset/choice behaviour is done; no universal filtering-cost or
older-GPU performance claim follows from the samples.

Implementation commit: `8405398`.

## Pictures and GPU timing limits

All paths above are under `F:\KK-native-renderer\`. Each item directory contains full-resolution
before/after PNGs, settings metadata, and sheets with reduced full views plus unenhanced native-pixel
centre crops. Final matrix identifiers:

| API/output | Filename prefix | Run folder suffix |
|---|---|---|
| Vulkan 1440p | `vulkan-2560x1440-tail0-205254` | `task121-vulkan-2-tail0-20261015-205254` |
| Vulkan 4K | `vulkan-3840x2160-tail0-204810` | `task121-vulkan-3-tail0-20261015-204810` |
| D3D12 1440p | `d3d12-2560x1440-tail0-210625` | `task121-d3d12-2-tail0-20261015-210625` |
| D3D12 4K | `d3d12-3840x2160-tail0-205054` | `task121-d3d12-3-tail0-20261015-205054` |

For example, [D3D12 4K effect buffers](F:/KK-native-renderer/documentary/gfx-quick/2-effect-buffers/d3d12-3840x2160-tail0-205054-baseline-vs-effects.png)
and [D3D12 1440p CAS Medium](F:/KK-native-renderer/documentary/gfx-quick/4-cas/d3d12-2560x1440-tail0-210625-baseline-vs-cas-medium.png).

Quality isolation used chapter 8, a fixed camera, 60 cap, AO/GI/new lighting off, and the existing
development forced-step hook at 0.000001 s after 30 s. No production timing code changed. Rain/fire
still differ between sequential captures. Three-shot bursts quantify this noise; visual differences
alone do not establish a useful effect or temporal stability. All final metadata says preparing=0
and confirms output dimensions and actual SDK names. All eight runs used identical renderer DLLs.

Whole-frame GPU query observations, before -> after in milliseconds:

| Control | Vulkan 1440p | Vulkan 4K | D3D12 1440p | D3D12 4K |
|---|---:|---:|---:|---:|
| DLSS old bias -> auto | 10.55 -> 11.31 | 13.07 -> 13.59 | 9.81 -> 10.46 | 12.91 -> 13.59 |
| FSR old bias -> auto | 15.27 -> 15.42 | 15.31 -> 15.35 | 13.51 -> 14.12 | 10.17 -> 10.78 |
| XeSS zero bias -> auto | 15.56 -> 15.58 | 16.76 -> 18.09 | 15.64 -> 15.72 | 13.29 -> 13.92 |
| Original -> Output effects | 10.55 -> 12.00 | 13.07 -> 14.61 | 9.81 -> 11.28 | 12.91 -> 8.29 |
| 8x -> 16x | 10.55 -> 14.87 | 13.07 -> 15.61 | 9.81 -> 13.33 | 12.91 -> 10.24 |

These elapsed full-frame intervals drift across phases and do **not** isolate incremental feature
cost. The D3D12 decreases demonstrate the confounding. Queue/pacing, changing scene work and GPU
clocks were not controlled. Do not quote the differences as causal costs or speedups. Items 1/2
remain partial on performance acceptance; only CAS has an additional scoped pass measurement.

Evidence: `out/task121/evidence.json`, `run-audit.json`, `runtime-check.json`, and each private run's
`gpu.frame.csv`/`gpu.cas.csv`, arguments, captures and logs. Incomplete final CSV records from bounded
process stops were ignored. The analysis helpers are retained in ignored `out/task121/`.

## Eight launches, checks and handoff

All folders below are under `F:\KK-native-renderer\runs\`. Folder stamps are preparation times;
the launch column records actual acquisition/start. The first two runs are diagnostics, excluded
from the final quality/cost matrix because sampler-cache stalls compressed or skipped capture phases.
The development harness was corrected to preserve warmup and spacing after stalls.

| Folder suffix | Actual launch (local) | PID | Seconds | Raw frames | Purpose |
|---|---|---:|---:|---:|---|
| `task121-vulkan-2-tail0-20261015-204217` | 20:44:49 | 43948 | 139.41 | 45 | Diagnostic |
| `task121-d3d12-2-tail0-20261015-204610` | 20:47:17 | 9916 | 131.15 | 39 | Diagnostic |
| `task121-vulkan-3-tail0-20261015-204810` | 20:49:42 | 48836 | 151.27 | 45 | Final 4K |
| `task121-vulkan-2-tail0-20261015-205254` | 21:02:55 | 24732 | 133.13 | 45 | Final 1440p |
| `task121-d3d12-3-tail0-20261015-205054` | 21:17:26 | 20404 | 130.90 | 45 | Final 4K |
| `task121-vulkan-1-tail1-20261015-211819` | 21:21:50 | 48308 | 199.63 | 21 | Golden, tails On |
| `task121-d3d12-2-tail0-20261015-210625` | 21:29:57 | 41560 | 130.91 | 45 | Final 1440p |
| `task121-d3d12-1-tail1-20261015-211958` | 21:32:29 | 48752 | 196.16 | 21 | Golden, tails On |

Exactly eight game launches; no ninth. One additional folder was prepared by a cancelled waiting
helper and never launched. A PowerShell array-serialization issue in the private ledger was corrected
after launch three, preserving all three records; the final ledger and PID/capture audit confirm eight.
All four final matrix runs have 45/45 captures at their requested dimensions. Both golden runs have
21/21 captures. Only recorded owned PIDs were stopped. No task-121 game/helper remains running.

Every launch used the shared atomic lock, priority/user-game checks, already-running kkbg2, private
build/userdata/cache/log roots, this worktree's asset link, SDL activation hints at 0, hidden start,
fullscreen=false, 1280x720 window arguments and desktop fullscreen mode. No focus operation or desktop
screenshot was used. Other jobs' games, locks and priority reservations were left alone. No user
Documents/Desktop game/save/cache was accessed or changed. No files or caches were deleted.

| Check | Result |
|---|---|
| Development game build, both APIs/upscalers enabled | PASS |
| Standalone native build/CTest | **35/35 PASS**, 160.29 s |
| Shader build/CTest, including GPU differential test | **23/23 PASS**, 492.24 s |
| Resources build/CTest | **2/2 PASS**, 4.01 s |
| Development/launcher CTest, after final preset-label change | **9/9 PASS**, 2.35 s |
| Total configured CTests | **69/69 PASS** |
| Final capture/PID/dimension/ready-state/DLL audit | PASS |
| git diff --check | PASS |

The initial CTest run caught the CAS wiring defect and a missing test-process runtime DLL search
path. Both were corrected before the passing gates. NVRHI test checks pass; Khronos validation is
not installed, so this is not a Khronos-validation-clean claim.

Both private API packs were rebuilt with scale/motion/lighting variants: 9,706 shaders compiled,
zero failures/parse failures and 17,910 indexed entries each. The CLI packs do not add the three XEX
built-in containers; runtime DXC was included for those/misses. These are private developer inputs,
not complete distributable prepared packs. DLSS/XeSS strings and all six vendor runtime DLLs were
verified; DXC and glyphs were copied into each run. SDK/upscaler/asset links point at permanent
locations. Build-generated manifest/rexglue metadata was restored.

The final preset-label change affects UI recognition only, was unit-tested and rebuilt after the
capture binaries were staged, and changes no captured rendering. Implementation commits are listed
per item; the fifth commit contains this report and the mip-tail validation record. All commits
carry the required Claude co-author trailer.

No distributable/test-build package was handed over, so the package/global-cache cleanup checklist
was not triggered. Recorded-route benchmarking and an independent review were not performed by this
helper; Claude's integration verification remains necessary. No downloads, installations, Linux work,
subagents, game-derived data commits, push, merge or release. Stop after this report.
