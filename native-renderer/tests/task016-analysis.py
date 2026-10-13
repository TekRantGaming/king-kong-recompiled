"""Task 016 comparison sheets from game-owned captures and AO regression images."""
from pathlib import Path
import json
import re
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

RUNS = Path(r'F:\KK-native-renderer\runs')
OUT = Path(r'F:\KK-native-renderer\analysis\ao\b4')
OUT.mkdir(parents=True, exist_ok=True)


def latest(pattern):
    paths = sorted(RUNS.glob(pattern))
    return paths[-1] if paths else None


def shot(run, seconds):
    fine = run / f'shot_ms_{round(seconds*1000)}.bmp'
    return fine if fine.exists() else run / f'shot_{int(seconds*10)}.bmp'


def sheet(name, rows, crop=None, size=(640, 360)):
    canvas = Image.new('RGB', (len(rows[0]) * size[0], len(rows) * (size[1] + 32)), (25, 25, 25))
    draw = ImageDraw.Draw(canvas)
    for y, row in enumerate(rows):
        for x, (label, path) in enumerate(row):
            draw.text((x * size[0] + 8, y * (size[1] + 32) + 8), label, fill='white')
            if path and path.exists():
                frame = Image.open(path).convert('RGB')
                if crop:
                    frame = frame.crop(crop)
                canvas.paste(frame.resize(size), (x * size[0], y * (size[1] + 32) + 32))
    canvas.save(OUT / name)


metrics = {}
for folder in sorted(RUNS.glob('task016-*')):
    log = folder / 'run.log'
    if not log.exists():
        continue
    text = log.read_text(errors='replace')
    frames = re.findall(r'AO frame (\d+): apply_point=(\d) computed=(\d) applied=(\d) reason=(.*?) phase=(\d).*?opaque=(\d+)', text)
    scene = [f for f in frames if int(f[5]) >= 2 and int(f[6]) > 0]
    metrics[folder.name] = {
        'scene_frames': len(scene),
        'not_applied': sum(f[3] == '0' for f in scene),
        'missing_reasons': sorted(set(f[4] for f in scene if f[3] == '0')),
        'gpu_ms': [float(t) for t in re.findall(r'AO GPU time \(gtao\): ([\d.]+) ms', text)],
        'denoise': re.findall(r'AO denoise frame .*', text)[-5:],
    }

for entry in range(2, 9):
    before = latest(f'task016-before-e{entry}-*')
    after = latest(f'task016-after-e{entry}-*')
    if before and after:
        rows = [[(f'Entry {entry}, {t/10:g}s, before', before / f'shot_{t}.bmp'),
                 (f'Entry {entry}, {t/10:g}s, after', after / f'shot_{t}.bmp')]
                for t in (90, 180, 270, 390)]
        sheet(f'jungle-e{entry}.png', rows)
        sheet(f'jungle-crops-e{entry}.png', rows, crop=(200, 180, 840, 540), size=(640, 360))

for entry in (2,8):
    before = latest(f'task016-before-dlss-e{entry}-*')
    after = latest(f'task016-after-dlss-e{entry}-*')
    if before and after:
        rows = [[(f'DLAA entry {entry}, {t/10:g}s, before', before / f'shot_{t}.bmp'),
                 (f'DLAA entry {entry}, {t/10:g}s, after', after / f'shot_{t}.bmp')]
                for t in (90,180,270,390)]
        sheet(f'jungle-dlss-e{entry}.png', rows)
        sheet(f'jungle-dlss-crops-e{entry}.png', rows, crop=(200,180,840,540), size=(640,360))

hands = [(label, latest(f'task016-{label}-e8-*'))
         for label in ('before-hands', 'low-hands', 'medium-hands', 'high-hands', 'dlss-hands', 'fsr-hands', 'xess-hands')]
hands = [(label, run) for label, run in hands if run and shot(run,39).exists()]
if hands:
    sheet('hands-full.png', [[(label, shot(run,39)) for label, run in hands]], size=(640,360))
    sheet('hands-crops.png', [[(label, shot(run,39)) for label, run in hands]],
          crop=(650,360,1200,700), size=(550,340))
    sheet('ao-surface-crops.png', [[(label, shot(run,39)) for label, run in hands]],
          crop=(220,210,280,300), size=(320,360))
    for label, run in hands:
        patch = Image.open(shot(run,39)).convert('L').crop((220,210,280,300))
        residual = np.asarray(patch, dtype=float) - np.asarray(patch.filter(ImageFilter.GaussianBlur(2)), dtype=float)
        metrics[run.name]['wall_highpass_std_0_255'] = float(residual.std())
        times = (39,39.034,39.068,39.102,39.136) if (run / 'shot_ms_39000.bmp').exists() else (39,39.1,39.2,39.3,39.4)
        frames = [np.asarray(Image.open(shot(run,t)).convert('RGB'), dtype=float)[360:700,650:1200]
                  for t in times if shot(run,t).exists()]
        if len(frames) > 1:
            metrics[run.name]['hands_crop_mae_0_255'] = float(np.mean([np.abs(b-a).mean() for a,b in zip(frames,frames[1:])]))
            metrics[run.name]['hands_crop_frames'] = len(frames)
            sheet(f'hands-sequence-{label}.png', [[(f'{label}, {t:g}s', shot(run,t))
                  for t in times]], crop=(650,360,1200,700), size=(550,340))

synthetic = OUT / 'synthetic'
old = OUT / 'synthetic-before'
if (old / 'game_ao_missing_end_water.ppm').exists():
    sheet('missing-fog-regression.png', [[('Synthetic missing end: before fix', old / 'game_ao_missing_end_water.ppm'),
          ('Synthetic missing end: after fix', synthetic / 'game_ao_missing_end_water.ppm')]], size=(768,432))
if (synthetic / 'gtao_temporal_before.ppm').exists():
    sheet('temporal-regression.png', [[('Rotating samples, spatial only', synthetic / 'gtao_temporal_before.ppm'),
          ('Same samples, local temporal', synthetic / 'gtao_temporal_after.ppm')]], size=(768,432))
if (old / 'game_ao_resolved_depth_before_water.ppm').exists():
    sheet('cleared-depth-regression.png', [[('Synthetic cleared depth: before fix', old / 'game_ao_resolved_depth_before_water.ppm'),
          ('Synthetic cleared depth: after fix', synthetic / 'game_ao_resolved_depth_before_water.ppm')]], size=(768,432))
(OUT / 'metrics.json').write_text(json.dumps(metrics, indent=2))
print(json.dumps(metrics, indent=2))
