from pathlib import Path
import datetime as dt
import hashlib
import json
import re

run=Path('F:/KK-native-renderer/runs/task069-20261015-015007/d3d12')
lines=(run/'run.log').read_text(encoding='utf-8').splitlines()
timestamp=lambda line: dt.datetime.fromisoformat(line[1:24])
origin=next(timestamp(line) for line in lines if 'UI repaint test phase 0 at' in line)
rows=[]
for name,lower,upper in [('legacy-capped',2,10),('on-demand-capped',14,22),('legacy-uncapped',26,34),('on-demand-uncapped',38,46)]:
    row={'phase':name,'window':[lower,upper]}
    for kind,pattern in [('host',r'UI paint trace: (\d+) host presents in ([\d.]+) s'),('guest',r'UI repaint guest trace: (\d+) guest swaps in ([\d.]+) s')]:
        samples=[]
        for line in lines:
            match=re.search(pattern,line)
            if not match: continue
            count,seconds=int(match[1]),float(match[2])
            end=(timestamp(line)-origin).total_seconds()
            if end-seconds>=lower and end<=upper:
                samples.append({'count':count,'seconds':seconds,'end':end})
        count=sum(sample['count'] for sample in samples)
        seconds=sum(sample['seconds'] for sample in samples)
        row[kind]={'count':count,'seconds':seconds,'rate':count/seconds if seconds else None,'samples':samples}
    rows.append(row)
print(json.dumps(rows,indent=2))
print('UI events:')
for line in lines:
    if 'KK pause:' in line or 'KK graphics panel:' in line:
        print(line)
print('Capture hashes:')
for path in sorted(Path('F:/KK-native-renderer/wt/codex-uipaint/documentary/uipaint').glob('*.png')):
    print(path.name,hashlib.sha256(path.read_bytes()).hexdigest())
