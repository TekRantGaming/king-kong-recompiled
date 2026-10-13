"""Render the task report from preserved test metadata and counters."""
import json
import runpy
import re
import statistics
import subprocess
import sys
from pathlib import Path

if len(sys.argv)>1 and sys.argv[1]=='--check':
    print('Report generator syntax checked')
    raise SystemExit(0)

summary=runpy.run_path(str(Path(__file__).with_name('perf-012-summary.py')))['summary']
root=Path('F:/KK-native-renderer/runs')
rows=[]
for folder in sorted(root.glob('task012-*')):
    if (folder/'test.json').exists() and (folder/'process.csv').exists():
        rows.append(summary(folder))

def n(value, digits=2):
    return f'{value:.{digits}f}' if value is not None else 'n/a'

def table(headers, data):
    return '\n'.join(['| '+' | '.join(headers)+' |','|'+'|'.join(['---']*len(headers))+'|']+
                     ['| '+' | '.join(map(str,row))+' |' for row in data])

def status(r):
    if r.get('device_removed'): return 'device removed'
    if r.get('volatile_errors'): return f"invalid: {r['volatile_errors']} exhaustion errors"
    if r.get('query_errors'): return 'invalid: query pool'
    if r.get('drawless'): return 'invalid: no geometry'
    if r.get('shots') != 2: return 'incomplete'
    return 'complete'

def api(r): return 'Classic' if r['classic'] else r['backend']

def selected(prefix): return [r for r in rows if r['label'].startswith(prefix)]

report=['# Task 012: native renderer optimisation review',
'''\nWorktree: `F:\\KK-native-renderer\\wt\\codex-perf`, branch `codex-perf` from `beta4`.
Machine: RTX 4070 Ti, 12,282 MiB dedicated VRAM, NVIDIA driver 617.42; Ryzen 7 7800X3D, eight physical cores / sixteen logical processors; about 63.7 GiB visible system RAM. Date: 13 October 2026.

The proven failure is unbounded unfinished GPU work. In the saved 4K firing control, Vulkan exhausted volatile constant versions, while D3D12 grew its upload allocations to about 18.3 GB private bytes / 14.8 GB shared GPU memory, then reported device removal. The renderer previously bounded CPU submissions, but not GPU completion. The fixes below bound frame reuse, reduce repeated stream uploads and scale-constant writes, and reject unsupported Vulkan devices clearly.

## Method and limits

Read backend.md, its open problems, testing.md, chapter-jump.md and the saved gun-fire task before targeted source review. Started with V-Rex 8, Venture 0 and Kong to the Rescue 27, native Vulkan versus Classic. Expanded the resolution/effects matrix in V-Rex after reproducing the problem, with final basic checks in the other two chapters. This is not a 42-chapter performance certification.

All games used private copied executables, DLLs, glyphs, shader packs, DXC and user data in `runs/task012-*`; the worktree's assets; the shared `runs/codex-game.lock`; both SDL activation variables set to zero; the kkbg2 helper; window arguments 1280x720, fullscreen false, fullscreen mode desktop. Only own PIDs were closed/stopped. No exclusive fullscreen, desktop screenshots, user saves, push, merge or release. Each case targets 120 seconds on the loaded-level script clock and has a 240-second process limit. Venture includes its approximately 60-second load. No own game run exceeded ten minutes. Lock contention caused substantial wall-clock waiting.

Original look was enabled for a controlled picture, with frame cap, native scale, temporal upscaler and AO explicitly retained/overridden. Frame rate was uncapped. 1080p and 1440p refer to internal native render sizes in the required 1280x720 window. Classic's draw scales are integers, so an exact 1080p internal render comparison is unavailable without changing the Classic renderer. Classic has spatial presentation effects and legacy SSAO, not this native GTAO or DLSS path; those are not interchangeable.

Host medians/p95 use game-frame CSV samples at script seconds 80-120. CPU execution uses Windows thread times, excluding sleeps; render/host means use late counter deltas. GPU values are nonblocking timestamp averages; whole-frame timing excludes the SDK's final host-surface composition. Pass profiling samples every fourth frame and groups consecutive draws by render/depth target pair, with uploads included. Per-group averages are conditional on that group occurring. GPU timing varied on this actively used PC, so byte/call reductions are stronger evidence than small timing differences. Driver disk caches were already warm; per-run application caches were private/fresh. A genuinely cold NVIDIA driver cache was not measured.

Software graphics-state calls and game-pipeline transitions are reported below. Later profiles additionally count actual NVRHI pipeline binds (graphics and compute), viewport/scissor/blend/stencil emissions and D3D12 graphics root-CBV updates. These exclude SDK commands made directly outside NVRHI and do not count every resource barrier or descriptor-set bind. Earlier profiles have only the software proxies. Actual Classic GPU pass timings and Classic render/submit/present thread execution were not available from the existing SDK counters, and are marked unavailable. No GPU timing or hardware result is inferred for them.

## Initial comparison
''']
baseline=[r for r in rows if r['label'].startswith('before-')]
report.append(table(['Case','Chapter','API','Internal size','Host p50/p95 ms','GPU ms','Dedicated MiB','Seconds','Result'],
 [(r['label'],r['chapter'],api(r),r['scale'],n(r.get('steady_p50'))+'/'+n(r.get('steady_p95')),n(r.get('gpu_ms')),
   n(r.get('dedicated_mb'),0),n(r.get('seconds'),1),status(r)) for r in baseline]))
report.append('''\nThe first `baseline-vk-1080` launch used the default autoskip sequence and opened pause screens, so it is excluded as a controlled gameplay comparison. Old error-producing CPU frame times are not valid rendered-throughput measurements. Error counts are counts in the retained rotating logs, not guaranteed lifetime totals.

## Basic renderer checks
''')
basic=selected('final-')+selected('ticket-')
report.append(table(['Case','Chapter','API','Size','Host p50/p95 ms','Game CPU ms','Render CPU ms','Host submit CPU ms','Refresh CPU ms','Frame GPU ms','Result'],
 [(r['label'],r['chapter'],api(r),r['scale'],n(r.get('steady_p50'))+'/'+n(r.get('steady_p95')),
   n(r.get('game_cpu_ms')),n(r.get('render_cpu_ms')),n(r.get('submit_cpu_ms')),n(r.get('present_cpu_ms')),
   n(r.get('gpu_ms')),status(r)) for r in basic]))
report.append('''\nHost submission CPU includes both native frame and output-copy submissions; refresh CPU overlaps that subset. Do not add those columns as disjoint costs. CPU counters were added after the initial comparison. `ticket-*` cases, where present, repeat checks after replacing the blanket submission flush with the matching old-frame ticket.

## Upscalers and GTAO
''')
effects=selected('effects-')
report.append(table(['Case','API','Size','Host p50/p95 ms','Frame GPU ms','AO GPU ms','Upscale point GPU ms','SDK subset ms','Dedicated MiB','Result'],
 [(r['label'],api(r),r['scale'],n(r.get('steady_p50'))+'/'+n(r.get('steady_p95')),n(r.get('gpu_ms')),
   n(r.get('ao_ms')),n(r.get('upscale_ms')),n(r.get('upscale_sdk_ms')),n(r.get('dedicated_mb'),0),status(r)) for r in effects]))
report.append('''\nDLSS was requested at Quality with the shipped/latest preset, GTAO at strength 2 with the port's automatic resolution. The upscale-point total includes SDK work; its SDK subset is not an additional cost. Logs must confirm the requested SDK actually ran rather than a fallback. Missing AO/upscaler timings mean no completed timing samples, not a fabricated zero.

## Sustained 3x firing and fewer cores
''')
special=selected('gunfire-')+selected('affinity-')
report.append(table(['Case','API','Physical cores','Host p50/p95 ms','GPU ms','Private MiB','Dedicated/shared GPU MiB','Ammo consumed','Seconds','Result'],
 [(r['label'],api(r),r['cores'] or 8,n(r.get('steady_p50'))+'/'+n(r.get('steady_p95')),n(r.get('gpu_ms')),
   n(r.get('private_mb'),0),n(r.get('dedicated_mb'),0)+'/'+n(r.get('shared_mb'),0),r.get('ammo_consumed',0),
   n(r.get('seconds'),1),status(r)) for r in special]))
report.append('''\nFiring uses the saved task 009 dev ammo-consumption/refill helper and held-down aim/pad settings, to exercise impacts rather than silently run dry. The control loads the saved pre-fix native DLL with the current exe; metadata records its SHA256. Four/six-core affinity uses the Windows physical-core topology, masks 0xFF / 0xFFF, rather than assuming logical processors are cores. This does not emulate an older CPU's IPC, cache or memory system. 3x means a 3840x2160 draw in a window, not the user's exclusive 3840x2160@120 presentation path.

## Per-frame work and memory
''')
work=basic+effects+[r for r in special if r['label'].startswith('gunfire-after')]
report.append(table(['Case','Chapter','Draws','Graphics-state calls','Game pipeline transitions','Descriptor updates','Guest upload MiB','Buffer handling ms','Pipelines','Private/working MiB'],
 [(r['label'],r['chapter'],n(r.get('draws'),1),n(r.get('graphics_states'),1),n(r.get('pipeline_changes'),1),
   n(r.get('descriptor_updates'),2),n(r.get('upload_bytes',0)/2**20 if 'upload_bytes' in r else None),
   n(r.get('buffer_ms'),3),r.get('pipelines','n/a'),n(r.get('private_mb'),0)+'/'+n(r.get('working_mb'),0)) for r in work]))
report.append('''\nUpload bytes cover guest textures, vertices, indices and draw constants, not every auxiliary-pass allocation. Texture/buffer counts and late descriptor updates show steady cache reuse; residency counts are not byte-accurate VRAM accounting. WDDM dedicated/shared process usage and private/working-set bytes are different measures and are not interchangeable or additive unique-RAM estimates.

## Emitted commands
''')
report.append(table(['Case','NVRHI pipeline binds','Viewport commands','Scissor commands','Blend commands','Stencil commands','D3D graphics CBV updates'],
 [(r['label'],n(r.get('actual_pipeline_binds'),1),n(r.get('emitted_viewports'),1),n(r.get('emitted_scissors'),1),
   n(r.get('emitted_blends'),1),n(r.get('emitted_stencil'),1),n(r.get('emitted_root_cbv'),1)) for r in work if 'actual_pipeline_binds' in r]))
report.append('## GPU pass groups')
pass_rows=[]
for r in basic+[v for v in rows if v['label'] in ('scale-before3-vk-1440','scale-after-vk-1440')]:
    for name,values in r.get('gpu_passes',{}).items():
        pass_rows.append((r['label'],r['chapter'],api(r),name,n(statistics.mean(values[-3:]),3)))
report.append(table(['Case','Chapter','API','Pass group','GPU ms per sampled active frame'],pass_rows))
report.append('''\nTarget addresses identify the actual grouped passes in that process, not stable cross-run scene IDs. Scene draws, post draws and shadow targets can be distinguished in the frame/target logs; a single pair may be reused by several game passes. AO and upscale costs are separately timed by existing port counters. These groups include GPU uploads and may include nested AO/upscale work, so they should not be summed with the AO/upscale totals as separate costs.

## Proven changes and observations

- GPU completion bound: before, V-Rex 1440p retained 155,066 constant-buffer exhaustion errors and exited at 58.7 seconds; after the bound, the 122.9-second test completed with both captures and zero errors. Two per-image completion queries protect actual reuse. Query assignment is serialized with native host submissions. Vulkan semaphore waiting uses NVRHI bookkeeping serialization without taking the SDK presenter queue lock. The first candidate's median was 17.88 ms; removing that extra presenter lock reduced it to 9.19 ms.
- Repeated vertex streams: a cache scoped to one draw reuses only identical physical address, size and used byte range. V-Rex guest uploads fell from 11,765,732 to 9,725,689 bytes/frame, buffer handling from 0.712 to 0.467 ms, about 2,233 repeated binds/frame avoided. Host median stayed approximately 9.2 ms. Data rewritten between draws is still uploaded.
- Scale constants: compare all 528 bytes, while forcing the first write in every command list/frame, just as VS/PS constants already do. Writes fell from 905.057 to 41.747/frame, uploads from 9,721,315 to 9,261,065 bytes/frame, render CPU from 4.927 to 4.792 ms. Host median was 9.175 versus 9.169 ms. The detailed GPU sample varied upward, 4.153 to 5.211 ms, so no GPU speed improvement is claimed for this change. Later regular profiles are in the tables above.
- Unsupported Vulkan devices: validate nine enabled bindless features plus actual descriptor set/per-stage/pool limits before table creation. Fixed tables require seven sets, 18,944 sampled-image descriptors, 16,384 storage-buffer descriptors and 1,024 samplers. Missing features/limits log their names and actual requirements with a Native D3D12/Classic recommendation. Unverifiable old SDK feature sets are rejected. A requested game renderer that cannot initialize now fails rather than displaying the placeholder. Twenty-one feature/boundary tests pass; this GPU also starts through the guards.
- Submission overlap: the first completion implementation flushed newer frame submissions when recycling an older image. Venture exposed the regression: 4.18 ms before, 9.09 ms after that blanket flush, with approximately unchanged GPU cost. The matching-query submission ticket preserves the two-frame GPU bound while waiting only for the old frame's query to have been assigned. The final Venture repeat measured 6.493 ms median with zero errors; GPU mean varied from 3.163 to 5.194 ms, so this is not an isolated FPS gain. The unbounded initial Venture result remains faster, and the remaining queue/completion cost needs further profiling. Repeated measurements are identified as `ticket-*` above; do not substitute intermediate measurements for the final DLL.
- Diagnostics: reuse task 009 counters/helper, add thread CPU execution, WDDM memory, state/transition/descriptor counters and bounded GPU pass queries. CPU probes are off outside the explicit debug bit; game CSV probes require the dev build and log window. Pass timing needed 2,048 queries. Two early pass-profile attempts used an already occupied draw-stop bit and are discarded; bit 524288 is the corrected flag. The known-good 32768-only baseline/cache runs were unaffected.

Visual review: the game's own V-Rex 100-second captures before and after scale reuse show the expected scene without obvious missing objects or large colour changes. Rain/animation/settling differ, and thumbnail review is not a pixel-perfect rendering proof. Original captures are retained. No game images, packs or logs were committed.

## Hardware risks and recommendations not implemented

| Class | Evidence / remaining risk | Recommendation |
|---|---|---|
| 6 GB / 8 GB discrete GPU | Measured representative dedicated usage is in the tables; the old D3D12 failure was primarily massive shared/system upload growth. Whole-tour/scaled-target growth is not certified. | Keep GPU completion bounded; add live budget accounting and safe target/framebuffer eviction after proving resolve/alias lifetime. Retained EDRAM target/framebuffer caches can accumulate across chapters. |
| Older NVIDIA / AMD / Intel Vulkan | API 1.3, dynamic rendering, timeline semaphores, synchronization2, enabled indexing features and large descriptor limits are required. Features alone were insufficient. | Clear startup rejection is implemented. Test actual vendor hardware; consider smaller bindless capacities or a supported alternative binding path. Audit formats and optional sampler/raster capabilities before claiming universal support. |
| Older D3D12 GPU | Only RTX 4070 Ti tested. Large descriptor heaps, DXIL/shader-model support and driver behavior still matter. | Add explicit resource-binding-tier/shader-model/format diagnostics and verify actual older hardware. No D3D12 capability result is inferred from Vulkan feature tests. |
| Four/six-core CPU | Affinity measurements appear above; pipeline workers still derive their count from hardware_concurrency, which ignores a reduced process affinity mask. Device snapshots and declaration/index conversion allocate/copy on hot paths. | Respect affinity in compiler worker sizing; profile and reuse declaration/index scratch storage or snapshot pools with lifetime tests. Do not skip the original guest D3D functions without validating their state/fence side effects. |
| Integrated GPU / low system RAM | No integrated GPU available. Shared bandwidth, large descriptor arrays, guest aliases/working-set accounting and SDK support differ. The unbounded D3D12 control reached approximately 17 GiB private allocation even on this 64 GB PC. | Test UMA budgets and memory bandwidth explicitly. Prefer conservative render scale/AO settings and a supported fallback; dedicated-card memory numbers do not certify UMA behavior. |
| Pipeline compilation | Application caches were fresh but the NVIDIA driver cache was warm. Pipeline waits/creation and sample counts are retained in every run.log. The tour's documented shipped list has 3,214 pipelines at scale 1. | Record/prewarm the scaled, upscaler and AO variants actually used; test a genuinely cold driver cache and stutter during traversal. Do not silently replace pending real draws with unrelated placeholders. |
| Upscaler fallback | DLSS is the requested RTX test path; AMD/Intel paths and repeated SDK evaluation failures were not hardware-tested. Existing setup/failure paths log fallbacks. | Verify FSR/XeSS on their hardware and consider releasing/disabling an SDK after persistent failures rather than paying motion/preparation cost forever. |

Other scoped observations: sampler/view/framebuffer/pipeline caches already avoid repeated creation in steady scenes. Per-draw declaration vectors and converted-index vectors remain allocation opportunities. Write watches during Venture/movie decoding deserve a lock/unlock-aware upload path. Conditional rendering/occlusion surveys remain ignored. These were not changed without matched evidence and lifetime validation.

Exclusive presentation review: kk/src/display_modes.cpp selects a display mode through the SDK only for exclusive fullscreen; the native D3D12 hand-over remains a texture copy with explicit transitions, and Vulkan a blit with layout barriers. SDK presenter documentation notes that composition can impose vertical-sync waits even with sync interval zero. Consequently the 3x draw test measures the renderer workload but does not validate exclusive flip/tearing/refresh behavior. No display mode was changed. At 120 Hz the total budget is 8.33 ms; compare that with the measured 3x GPU demand before expecting 120 FPS without an upscaler or reduced work.

## Validation and retained evidence

`kk/build.bat kk-dev-vulkan` passed after the changes. `native-renderer/tests/check-requirements-012.bat` passed 21 cases. Runtime checks, captures, error counts and limitations are listed above. All fixes are separate local commits with before/after evidence; the report does not imply Claude has reviewed or accepted them.

Every measurement below links to its preserved run directory, including `test.json`, `frame_times.csv`, `process.csv`, `gpu-memory.csv`, the rotating native logs and the game's BMPs. Exact command arguments, DLL SHA256 and affinity mask are in metadata. Reproduce summaries with the absolute Python 3.12 executable, `-I`, and `native-renderer/tests/perf-012-summary.py`.
''')
report.append('## Pipeline stalls and queue waits')
stall_rows=[]
for r in work:
    stutter=' '.join(r.get('last_stutter',[]))
    match=re.search(r'(\d+) of (\d+) frames .*?, (\d+) ms in all;.*?worst wait ([\d.]+) ms',stutter)
    reuse=re.search(r'GPU frame reuse wait (\d+) ms over (\d+) frames',' '.join(r.get('last_reuse_wait',[])))
    stall_rows.append((r['label'],r['chapter'],match[1]+'/'+match[2] if match else 'n/a',
                       match[3] if match else 'n/a',match[4] if match else 'n/a',
                       n(r.get('host_queue_wait_ms')),n(int(reuse[1])/int(reuse[2])) if reuse and int(reuse[2]) else 'n/a'))
report.append(table(['Case','Chapter','Pipeline-wait frames / all','Total pipeline wait ms','Worst pipeline wait ms','Queue wait ms/frame','GPU reuse wait ms/frame'],stall_rows))
report.append('These are cumulative process counters, including startup, unlike the late steady-state CPU/GPU means. Queue waits are wall time, not CPU execution, and can overlap other threads. The low host CPU execution alongside queue/reuse wall waits explains why those waits deserve further presentation-path profiling.')
report.append('## Local commits')
commits=subprocess.check_output(['git','log','beta4..HEAD','--format=%h %s'],text=True).strip().splitlines()
report.append('\n'.join('- `'+line.split(' ',1)[0]+'`: '+line.split(' ',1)[1] for line in commits))
report.append('## Run directories')
for r in rows:
    report.append(f"- `{Path(r['path']).name}`: `{r['path']}`. {status(r)}.")
text='\n\n'.join(report)+'\n'
assert '\u2014' not in text
target=Path(sys.argv[1] if len(sys.argv)>1 else 'F:/KK-native-renderer/codex/REPORT-012.md')
assert str(target.resolve()).lower()==str(Path('F:/KK-native-renderer/codex/REPORT-012.md').resolve()).lower()
target.write_text(text,encoding='utf-8')
print(f'Wrote {target}: {len(rows)} run records')
