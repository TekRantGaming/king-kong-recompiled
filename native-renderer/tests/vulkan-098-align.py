import json
import math
from pathlib import Path
import runpy
import sys

import numpy as np
from PIL import Image

workspace = Path(__file__).resolve().parents[2]
run = Path(sys.argv[1])
compare = runpy.run_path(str(workspace / 'native-renderer/tests/compare.py'))
data = compare['load_scenes'](workspace / 'native-renderer/tests/scenes.json')
scenes = [s for s in data['scenes'] if s['name'] in ('kong_cutscene', 'kong')]
golden = Path('F:/KK-native-renderer/golden')
cache = {}

def small(path):
    if path not in cache:
        with Image.open(path) as im:
            cache[path] = np.asarray(im.convert('RGB').resize((160, 90)), dtype=np.float32)
    return cache[path]

goldens = {s['name']: compare['image_set'](golden / s['name']) for s in scenes}
garrays = {name: np.stack([small(p) for _, p in shots]) for name, shots in goldens.items()}
candidates = []
for half in range(-6, 31):
    offset = half / 2
    errors = []
    for scene in scenes:
        for t in scene['times']:
            p = run / 'game' / f'shot_{round((t + offset) * 10)}.png'
            errors.append(float(np.abs(garrays[scene['name']] - small(p)).mean(axis=(1, 2, 3)).min()))
    candidates.append({'offset_s': offset, 'rough_mae': sum(errors) / len(errors)})
candidates.sort(key=lambda x: x['rough_mae'])
offset = candidates[0]['offset_s']
result = {'method': 'One shared clock offset for both scenes, selected by lowest 160x90 nearest-golden MAE. Stock full-resolution thresholds unchanged. Diagnostic only; standard report retained.',
          'offset_s': offset, 'candidates': candidates, 'scenes': []}
for scene in scenes:
    name = scene['name']
    th = compare['thresholds_for'](data, scene)
    test = [(t + offset, run / 'game' / f'shot_{round((t + offset) * 10)}.png') for t in scene['times']]
    res = {'scene': name, 'thresholds': th, 'frames': [], 'notes': []}
    compare['score_frames'](res, goldens[name], test, th, scene.get('ignore'), None, name)
    passed = sum(not f['fails'] for f in res['frames'])
    need = math.ceil((th.get('min_pass') or 1) * len(test) - 1e-9)
    res.update(passed_frames=passed, needed_frames=need, status='pass' if passed >= need else 'fail')
    result['scenes'].append(res)
    print(f'{name}: offset {offset:+.1f}s, {passed}/{len(test)} pass, need {need}')
    for frame in res['frames']:
        print(Path(frame['test']).name, 'outside', round(frame['env_bad'], 6), 'tile', round(frame['env_tile'], 4), 'fails', frame['fails'])
out = run / 'alignment'
out.mkdir(exist_ok=True)
(out / 'summary.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
print('Evidence', out / 'summary.json')
