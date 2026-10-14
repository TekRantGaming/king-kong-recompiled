"""Read isolated task 012 logs without changing or copying game-derived data."""
import csv
import json
import re
import statistics
import sys
from pathlib import Path

def percentile(values, fraction):
    values = sorted(values)
    return values[min(len(values)-1, int((len(values)-1)*fraction))] if values else None

def mean(values):
    return statistics.mean(values) if values else None

def summary(root):
    config = json.loads((root/'test.json').read_text(encoding='utf-8-sig'))
    logs = []
    for path in root.glob('run*.log'):
        logs.extend(path.read_text(encoding='utf-8', errors='replace').splitlines())
    logs.sort()
    samples = []
    for line in logs:
        if 'Guest FPS:' in line:
            m = re.search(r'Guest FPS: ([\d.]+) \(([\d.]+) ms/frame', line)
            if m: samples.append(float(m[2]))
    frames = []
    steady_frames = []
    steady_cpu = []
    path = root/'frame_times.csv'
    if path.exists():
        with path.open() as f:
            for row in csv.DictReader(f):
                if float(row['t']) >= 10 and float(row['host_ms']) > 0:
                    frames.append(float(row['host_ms']))
                    if 80 <= float(row['t']) <= 120:
                        steady_frames.append(float(row['host_ms']))
                        if 'cpu_ms' in row: steady_cpu.append((int(row['frame']), float(row['cpu_ms'])))
    processes = list(csv.DictReader((root/'process.csv').open()))
    memory_path = root/'gpu-memory.csv'
    memory = list(csv.DictReader(memory_path.open())) if memory_path.exists() else []
    residency, perf, gpu, ao, upscale, upscale_sdk = [], [], [], [], [], []
    for line in logs:
        m = re.search(r'residency frame (\d+):.*per frame: ([\d.]+) draws, ([\d.]+) clears, ([\d.]+) resolves, ([\d.]+) guest upload bytes, ([\d.]+) ms textures, ([\d.]+) ms buffers; pipelines (\d+)', line)
        if m: residency.append([float(v) for v in m.groups()])
        m = re.search(r'perf frame (\d+): scale writes (\d+), identical (\d+); vertex binds (\d+), repeated (\d+); descriptor updates (\d+), graphics states (\d+), game pipeline transitions (\d+)', line)
        if m: perf.append([int(v) for v in m.groups()])
        for label, dest in [('frame GPU time',gpu),('AO GPU time',ao),('upscaler GPU time',upscale)]:
            if label in line:
                m = re.search(r': ([\d.]+) ms a frame on average', line)
                if m:
                    if label == 'upscaler GPU time' and 'everything at the upscale point' not in line:
                        upscale_sdk.append(float(m[1]))
                    else:
                        dest.append(float(m[1]))
    result = dict(path=str(root), **{k:v for k,v in config.items() if k!='args'})
    result.update(frame_samples=len(frames), host_p50=percentile(frames,.5), host_p95=percentile(frames,.95),
                  host_p99=percentile(frames,.99), fps_ms_last30=mean(samples[-30:]),
                  steady_samples=len(steady_frames), steady_p50=percentile(steady_frames,.5),
                  steady_p95=percentile(steady_frames,.95),
                  seconds=float(processes[-1]['seconds']) if processes else None,
                  private_mb=max((int(v['private_bytes'])/2**20 for v in processes),default=0),
                  working_mb=max((int(v['working_set'])/2**20 for v in processes),default=0),
                  dedicated_mb=max((float(v['dedicated_bytes'] or 0)/2**20 for v in memory),default=None),
                  shared_mb=max((float(v['shared_bytes'] or 0)/2**20 for v in memory),default=None),
                  gpu_ms=mean(gpu[-5:]), ao_ms=mean(ao[-5:]), upscale_ms=mean(upscale[-5:]),
                  upscale_sdk_ms=mean(upscale_sdk[-5:]),
                  volatile_errors=sum('maxVersions' in v and 'insufficient' in v for v in logs),
                  query_errors=sum('Insufficient query pool space' in v for v in logs),
                  device_removed=sum('Device Removed!' in v for v in logs),
                  shots=len({p.stem for ext in ('png', 'bmp') for p in root.glob('shot_*.' + ext)}))
    if len(steady_cpu) > 1:
        result['game_cpu_ms']=(steady_cpu[-1][1]-steady_cpu[0][1])/(steady_cpu[-1][0]-steady_cpu[0][0])
    render_cpu=[]
    host_cpu=[]
    result['gpu_passes']={}
    for line in logs:
        m=re.search(r'render CPU frame (\d+): (\d+) us',line)
        if m: render_cpu.append(tuple(map(int,m.groups())))
        m=re.search(r'host CPU over (\d+) frames: (\d+) us submission, (\d+) us presenter refresh',line)
        if m: host_cpu.append(tuple(map(int,m.groups())))
        m=re.search(r'pass GPU time \((.*?)\): ([\d.]+) ms',line)
        if m: result['gpu_passes'].setdefault(m[1],[]).append(float(m[2]))
    for dest,names in [(render_cpu,['render_cpu_ms']),(host_cpu,['submit_cpu_ms','present_cpu_ms'])]:
        if len(dest)>10:
            a,b=dest[-11],dest[-1]
            for i,name in enumerate(names,1): result[name]=(b[i]-a[i])/(b[0]-a[0])/1000
    recent=residency[-10:]
    if recent:
        for i,name in enumerate(['draws','clears','resolves','upload_bytes','texture_ms','buffer_ms'],1):
            result[name]=mean([v[i] for v in recent])
        result['pipelines']=int(recent[-1][7])
        result['drawless']=result['draws']==0
    recent=perf[-10:]
    if recent:
        for i,name in enumerate(['scale_writes','scale_identical','vertex_binds','vertex_repeated','descriptor_updates','graphics_states','pipeline_changes'],1):
            result[name]=mean([v[i]/300 for v in recent])
    emitted=[]
    for line in logs:
        m=re.search(r'emitted frame (\d+) \(300 frames\): (\d+) pipeline binds, (\d+) viewports, (\d+) scissors, (\d+) blends, (\d+) stencil refs, (\d+) root CBV updates',line)
        if m: emitted.append(list(map(int,m.groups())))
    if emitted:
        for i,name in enumerate(['actual_pipeline_binds','emitted_viewports','emitted_scissors','emitted_blends','emitted_stencil','emitted_root_cbv'],1):
            result[name]=mean([v[i]/300 for v in emitted[-10:]])
    result['last_native_summary']=[v for v in logs if 'game renderer:' in v][-1:]
    result['last_submit_summary']=[v for v in logs if 'hooks saw' in v][-1:]
    result['last_thread_summary']=[v for v in logs if 'render thread:' in v][-1:]
    result['last_reuse_wait']=[v for v in logs if 'GPU frame reuse wait' in v][-1:]
    result['last_stutter']=[v for v in logs if 'pipeline stutter so far:' in v][-1:]
    gun=[]
    for line in logs:
        m=re.search(r'KK dev gunfire:.*? (\d+) consumed',line)
        if m: gun.append(int(m[1]))
    result['ammo_consumed']=max(gun,default=0)
    if result['last_submit_summary']:
        m=re.search(r'(\d+) frames; presents: (\d+) ms copy, (\d+) ms close, (\d+) ms submit, (\d+) ms frame end; submissions: (\d+) \((\d+) ms waiting for the queue, (\d+) ms submitting, (\d+) ms collecting\)',result['last_submit_summary'][0])
        if m:
            values=list(map(int,m.groups()))
            for i,name in enumerate(['present_copy_wall_ms','present_close_wall_ms','present_enqueue_wall_ms','frame_end_wall_ms'],1):
                result[name]=values[i]/values[0]
            result['host_queue_wait_ms']=values[6]/values[0]
            result['host_submit_wall_ms']=values[7]/values[0]
            result['host_collect_wall_ms']=values[8]/values[0]
    return result

if __name__=='__main__':
    for arg in sys.argv[1:]:
        base=Path(arg)
        folders=[base] if (base/'test.json').exists() else sorted(base.glob('task012-*'))
        for root in folders:
            if (root/'test.json').exists() and (root/'process.csv').exists():
                print(json.dumps(summary(root)))
