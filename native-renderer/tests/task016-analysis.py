"""Task 016 comparison sheets from game-owned captures and AO regression images."""
from pathlib import Path
import json
import re
import numpy as np
from PIL import Image, ImageDraw

RUNS = Path(r'F:\KK-native-renderer\runs')
OUT = Path(r'F:\KK-native-renderer\analysis\ao\b4')
OUT.mkdir(parents=True, exist_ok=True)


def latest(pattern):
    paths = sorted(RUNS.glob(pattern))
    return paths[-1] if paths else None


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

hands = [(label, latest(f'task016-{label}-e8-*'))
         for label in ('before-hands', 'low-hands', 'medium-hands', 'high-hands', 'dlss-hands', 'fsr-hands', 'xess-hands')]
hands = [(label, run) for label, run in hands if run and (run / 'shot_390.bmp').exists()]
if hands:
    sheet('hands-full.png', [[(label, run / 'shot_390.bmp') for label, run in hands]], size=(640,360))
    sheet('hands-crops.png', [[(label, run / 'shot_390.bmp') for label, run in hands]],
          crop=(650,360,1200,700), size=(550,340))
    for label, run in hands:
        frames = [np.asarray(Image.open(run / f'shot_{t}.bmp').convert('RGB'), dtype=float)[360:700,650:1200]
                  for t in (390,391,392,393,394) if (run / f'shot_{t}.bmp').exists()]
        if len(frames) > 1:
            metrics[run.name]['hands_crop_mae_0_255'] = float(np.mean([np.abs(b-a).mean() for a,b in zip(frames,frames[1:])]))
            metrics[run.name]['hands_crop_frames'] = len(frames)
            sheet(f'hands-sequence-{label}.png', [[(f'{label}, {t/10:g}s', run / f'shot_{t}.bmp')
                  for t in (390,391,392,393,394)]], crop=(650,360,1200,700), size=(550,340))

synthetic = OUT / 'synthetic'
old = OUT / 'synthetic-before'
if (old / 'game_ao_missing_end_water.ppm').exists():
    sheet('missing-fog-regression.png', [[('Synthetic missing end: before fix', old / 'game_ao_missing_end_water.ppm'),
          ('Synthetic missing end: after fix', synthetic / 'game_ao_missing_end_water.ppm')]], size=(768,432))
if (synthetic / 'gtao_temporal_before.ppm').exists():
    sheet('temporal-regression.png', [[('Rotating samples, spatial only', synthetic / 'gtao_temporal_before.ppm'),
          ('Same samples, local temporal', synthetic / 'gtao_temporal_after.ppm')]], size=(768,432))
(OUT / 'metrics.json').write_text(json.dumps(metrics, indent=2))
print(json.dumps(metrics, indent=2))
