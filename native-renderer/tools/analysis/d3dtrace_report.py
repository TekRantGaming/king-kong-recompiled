# Summarises a KK_DEV_D3D_TRACE log: per-function call counts in the traced frames, and for each function
# the distinct values seen in each argument register (to work out the arguments' meaning).
# Usage: python d3dtrace_report.py <log> [function]
import re
import sys
from collections import Counter, defaultdict

log = sys.argv[1]
only = sys.argv[2] if len(sys.argv) > 2 else None
call_re = re.compile(r'KK d3d: (sub_[0-9A-F]{8}) lr=([0-9A-F]{8}) r3=([0-9A-F]{8}) r4=([0-9A-F]{8}) r5=([0-9A-F]{8}) '
                     r'r6=([0-9A-F]{8}) r7=([0-9A-F]{8}) r8=([0-9A-F]{8}) r9=([0-9A-F]{8}) r10=([0-9A-F]{8}) f1=(\S+)')
calls = []
frames = []
totals = None
for line in open(log, encoding='utf-8', errors='replace'):
    m = call_re.search(line)
    if m:
        calls.append(m.groups())
        continue
    if 'KK d3d: frame' in line and 'end:' in line:
        frames.append(line.split('end:', 1)[1].strip())
    elif 'totals since launch:' in line:
        totals = line.split('totals since launch:', 1)[1].strip()
print(len(calls), 'traced calls in', len(frames), 'frames')
for i, f in enumerate(frames):
    print('frame', i + 1, 'histogram:', f)
if totals:
    print('totals since launch:', totals)
print()
by = defaultdict(list)
for c in calls:
    by[c[0]].append(c)
order = sorted(by, key=lambda f: -len(by[f]))
for f in order:
    if only and f != only:
        continue
    rows = by[f]
    print('==', f, len(rows), 'calls; call sites:', dict(Counter(r[1] for r in rows).most_common(6)))
    for ai, name in enumerate(['r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10', 'f1'], start=2):
        vals = Counter(r[ai] for r in rows)
        if len(vals) == 1:
            print(f'   {name}: always {next(iter(vals))}')
        else:
            top = ', '.join(f'{v}x{n}' for v, n in vals.most_common(6))
            print(f'   {name}: {len(vals)} distinct: {top}')
    if only:
        for r in rows[:60]:
            print('   ', ' '.join(r[1:]))
