"""The setup's comparison close-ups (job J4): crops and pairs the frames tools/setup_art.ps1 captured.

Each option gets two 16:9 crops of the same place, <key>-original.png (the option off: the game as it was) and
<key>-new.png (on), written to <out> (a release package's setup-art folder beside king_kong.exe; never git).
The place is found by the numbers: the window (default 960x540 of the 2560x1440 frames) where the two frames
differ most, less four times what the scene changes by itself between two captures with the same settings (ref, ref2), so
the close-up shows the option, not the wind in the trees. Frames drawn at 1280x720 are scaled up 2x first
(smoothly, as a 720p picture on a larger screen). A summary (pairs.json) records the windows and the numbers.

Usage: python -I setup_art_crop.py --main <run> --low <run> --orig <run> --water <run> [--shafts <run>] --out <folder>
       [--size 960x540] [--copy <second folder>]
"""
import argparse
import json
import shutil
from pathlib import Path

import numpy as np
from PIL import Image

W, H = 2560, 1440


def frame(run, index, name):
    path = Path(run) / 'captures' / f'{index}-{name}-0.png'
    img = Image.open(path).convert('RGB')
    if img.size != (W, H):  # 1280x720: the picture a 720p frame gives on a larger screen
        img = img.resize((W, H), Image.BILINEAR)
    return img


def window_scores(a, b, drift, cw, ch, step):
    """Mean |a - b| (minus four times the scene's own change) over every cw x ch window, on a step grid."""
    d = np.abs(np.asarray(a, np.float32) - np.asarray(b, np.float32)).mean(axis=2)
    if drift is not None:
        d = d - 4.0 * drift
    ii = np.pad(d.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
    best = None
    for y in range(0, H - ch + 1, step):
        for x in range(0, W - cw + 1, step):
            s = (ii[y + ch, x + cw] - ii[y, x + cw] - ii[y + ch, x] + ii[y, x]) / (cw * ch)
            if best is None or s > best[0]:
                best = (float(s), x, y)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--main', required=True)
    ap.add_argument('--low', required=True)
    ap.add_argument('--orig', required=True)
    ap.add_argument('--water', required=True)
    ap.add_argument('--alt-main', default='', help='a main run at another spot: each image and lighting option uses '
                    'whichever of the two shows it more clearly')
    ap.add_argument('--shafts', default='', help='another water run for Light shafts (default: --water)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--size', default='960x540')
    ap.add_argument('--copy', default='')
    ap.add_argument('--skip', default='', help='keys left out (no spot shows them): the launcher shows its placeholder')
    args = ap.parse_args()
    cw, ch = (int(v) for v in args.size.split('x'))
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    main_ref, main_ref2 = frame(args.main, 0, 'ref'), frame(args.main, 9, 'ref2')
    water_ref, water_ref2 = frame(args.water, 0, 'ref'), frame(args.water, 3, 'ref2')
    shafts_run = args.shafts or args.water
    shafts_ref, shafts_ref2 = frame(shafts_run, 0, 'ref'), frame(shafts_run, 3, 'ref2')
    low = frame(args.low, 0, 'ref720')

    def drift(a, b):
        return np.abs(np.asarray(a, np.float32) - np.asarray(b, np.float32)).mean(axis=2)

    main_drift, water_drift = drift(main_ref, main_ref2), drift(water_ref, water_ref2)
    shafts_drift = drift(shafts_ref, shafts_ref2)
    pairs = {
        # key: (original, new, the scene's own change or None between launches)
        'resolution': (low, main_ref, None),
        'upscaler': (low, frame(args.main, 7, 'upscaler'), None),
        'smaa': (main_ref, frame(args.main, 3, 'smaa'), main_drift),
        'sharpening': (main_ref, frame(args.main, 4, 'sharpening'), main_drift),
        'filtering': (main_ref, frame(args.main, 5, 'filtering'), main_drift),
        'gi': (main_ref, frame(args.main, 1, 'gi'), main_drift),
        'ao': (main_ref, frame(args.main, 2, 'ao'), main_drift),
        'shadows': (main_ref, frame(args.main, 6, 'shadows'), main_drift),
        'look': (frame(args.orig, 0, 'original'), frame(args.main, 8, 'remastered'), None),
        'water': (water_ref, frame(args.water, 1, 'water'), water_drift),
        'shafts': (shafts_ref, frame(shafts_run, 2, 'shafts'), shafts_drift),
    }
    summary = {'size': [cw, ch], 'runs': {'main': args.main, 'low': args.low, 'orig': args.orig, 'water': args.water,
                                           'shafts': shafts_run},
               'scene_change': {'main': float(main_drift.mean()), 'water': float(water_drift.mean()),
                                'shafts': float(shafts_drift.mean())},
               'pairs': {}}
    alt = None
    if args.alt_main:
        alt_ref, alt_ref2 = frame(args.alt_main, 0, 'ref'), frame(args.alt_main, 9, 'ref2')
        alt = (alt_ref, drift(alt_ref, alt_ref2))
        summary['scene_change']['alt_main'] = float(alt[1].mean())
        summary['runs']['alt_main'] = args.alt_main
    phase = {'smaa': 3, 'sharpening': 4, 'filtering': 5, 'gi': 1, 'ao': 2, 'shadows': 6}
    skip = {k for k in args.skip.split(',') if k}
    for key, (a, b, dr) in pairs.items():
        if key in skip:
            summary['pairs'][key] = {'skipped': True}
            print(f'{key:11s} skipped')
            continue
        source = 'main'
        score, x, y = window_scores(a, b, dr, cw, ch, 40)
        if alt is not None and key in phase:
            b2 = frame(args.alt_main, phase[key], key)
            s2 = window_scores(alt[0], b2, alt[1], cw, ch, 40)
            if s2[0] > score:
                a, b, (score, x, y), source = alt[0], b2, s2, 'alt_main'
        whole = float(np.abs(np.asarray(a, np.float32) - np.asarray(b, np.float32)).mean())
        box = (x, y, x + cw, y + ch)
        a.crop(box).save(out / f'{key}-original.png', optimize=True)
        b.crop(box).save(out / f'{key}-new.png', optimize=True)
        summary['pairs'][key] = {'source': source, 'window': [x, y, cw, ch], 'difference_whole': round(whole, 3),
                                 'difference_window': round(score, 3)}
        print(f'{key:11s} whole {whole:6.2f}  window {score:6.2f} at {x},{y} ({source})')
    (out / 'pairs.json').write_text(json.dumps(summary, indent=1), encoding='utf-8')
    if args.copy:
        dest = Path(args.copy)
        dest.mkdir(parents=True, exist_ok=True)
        for f in out.iterdir():
            shutil.copy2(f, dest / f.name)
    print('scene change (ref vs ref2): main {:.3f}, water {:.3f}, shafts {:.3f}'.format(*summary['scene_change'].values()))


if __name__ == '__main__':
    main()
