# Summary of a graphics panel test run (tools/graphics-panel-run.ps1): checks, hitches, errors, memory.
import csv, re, sys, os

def summarize(run):
    log = open(os.path.join(run, 'run.log'), encoding='utf-8', errors='replace').read().splitlines()
    t0 = None
    start_line = None
    for i, l in enumerate(log):
        if 'KK panel test: start' in l:
            start_line = i
            break
    after = log[start_line:] if start_line is not None else []
    passes = [l for l in after if 'KK panel test: PASS' in l]
    fails = [l for l in after if 'KK panel test: FAIL' in l]
    done = [l for l in after if 'KK panel test: done' in l]
    toggles = sum(1 for l in after if re.search(r'toggle \d+ =', l)) * 25
    hitches = [int(m.group(1)) for l in after for m in [re.search(r'Hitch: (\d+) ms', l)] if m]
    errors = [l for l in after if ('[error]' in l or '[critical]' in l) and 'ResolvePath' not in l]
    warns = [l for l in after if '[warning]' in l and 'NtCreateFile' not in l and 'VFS' not in l]
    lost = [l for l in log if re.search(r'device (lost|removed)|DEVICE_LOST|DXGI_ERROR', l, re.I)]
    changes = [l for l in after if 'live settings: upscaler' in l]
    boundary = [float(m.group(1)) for l in changes for m in [re.search(r'\(([\d.]+) ms at the frame boundary', l)] if m]
    slow = [l for l in after if 'spent' in l and 'applying a change' in l]
    stats = [l for l in after if 'live settings frame' in l]
    worst = [l for l in after if 'Frame times:' in l][1:]  # the first window holds the level's load
    worst_ms = max([int(m.group(1)) for l in worst for m in [re.search(r'worst (\d+) ms', l)] if m] or [0])
    mem = []
    p = os.path.join(run, 'process-memory.csv')
    if os.path.exists(p):
        mem = list(csv.DictReader(open(p)))
    print('RUN', os.path.basename(run))
    print('  test finished:', bool(done), done[0].split('KK panel test: ')[1] if done else '')
    print('  checks: %d passed, %d failed' % (len(passes), len(fails)))
    for f in fails: print('   ', f.split('KK panel test: ')[1])
    print('  toggles logged (x25):', toggles)
    print('  upscaler rebuilds:', len(changes), ' frame-boundary ms max %.1f' % max(boundary or [0]))
    print('  slow change frames (>30 ms render thread):', len(slow))
    for l in slow[:5]: print('   ', l[l.find('rexgpu'):])
    print('  hitches >=50 ms during the test:', len(hitches), 'max', max(hitches or [0]), 'over 250:', sum(h > 250 for h in hitches))
    print('  worst frame in 10 s windows during the test: %d ms' % worst_ms)
    print('  errors:', len(errors), ' device lost lines:', len(lost), ' warnings:', len(warns))
    for l in (errors + lost)[:8]: print('   ', l[:220])
    for l in warns[:6]: print('   W', l[:220])
    if stats: print('  last stats:', stats[-1][stats[-1].find('frame'):])
    if mem:
        start = [int(r['private_mb']) for r in mem if int(r['seconds']) < 60]
        print('  private MB: first-minute max %s, end %s, max %s' % (max(start) if start else '?', mem[-1]['private_mb'], max(int(r['private_mb']) for r in mem)))

for r in sys.argv[1:]:
    summarize(r)
