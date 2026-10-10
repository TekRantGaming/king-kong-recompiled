# Compares texture images in pairs: kknr_texdump's PNGs against the same textures exported from today's
# renderer (RenderDoc "Save Texture" as PNG on the D3D12 or Vulkan capture). Brief 03's acceptance check.
#
# Usage: python texcompare.py <ours_dir> <theirs_dir> [--tolerance N] [--html report.html]
#   Pairs files by name (ours: <name>.png; theirs: <name>.png, or any file starting with <name>).
#   Per pair: size check, max and mean absolute difference per channel, PSNR, and the share of texels off by
#   more than the tolerance (default 2 / 255: BC interpolation and float rounding differ by 1 between
#   decoders). Exit code 1 when any pair fails.
#
# Needs numpy and Pillow.
import argparse
import os
import sys

import numpy as np
from PIL import Image


def load(path):
    return np.asarray(Image.open(path).convert('RGBA'), dtype=np.int16)


def compare(a, b, tolerance):
    if a.shape != b.shape:
        return {'ok': False, 'why': 'size %s vs %s' % (a.shape[1::-1], b.shape[1::-1])}
    d = np.abs(a - b)
    mse = float(np.mean((a - b).astype(np.float64) ** 2))
    psnr = float('inf') if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)
    off = float(np.mean(np.any(d > tolerance, axis=2)))
    return {
        'ok': off == 0.0,
        'max': [int(x) for x in d.reshape(-1, 4).max(axis=0)],
        'mean': [round(float(x), 3) for x in d.reshape(-1, 4).mean(axis=0)],
        'psnr': psnr,
        'off': off,
        'why': '' if off == 0.0 else '%.2f%% of texels off by more than %d' % (100 * off, tolerance),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ours')
    ap.add_argument('theirs')
    ap.add_argument('--tolerance', type=int, default=2)
    ap.add_argument('--html')
    args = ap.parse_args()
    theirs = sorted(os.listdir(args.theirs))
    rows = []
    for name in sorted(os.listdir(args.ours)):
        if not name.endswith('.png'):
            continue
        stem = name[:-4]
        match = [t for t in theirs if t == name] or [t for t in theirs if t.startswith(stem)]
        if not match:
            rows.append((stem, {'ok': False, 'why': 'no counterpart'}, ''))
            continue
        other = os.path.join(args.theirs, match[0])
        rows.append((stem, compare(load(os.path.join(args.ours, name)), load(other), args.tolerance), other))
    failed = 0
    for stem, r, _ in rows:
        failed += not r['ok']
        if 'max' in r:
            print('%-40s %s  max %s  mean %s  psnr %.1f  %s' % (stem, 'ok  ' if r['ok'] else 'FAIL', r['max'], r['mean'],
                                                             r['psnr'], r['why']))
        else:
            print('%-40s FAIL  %s' % (stem, r['why']))
    print('%d pairs, %d failed' % (len(rows), failed))
    if args.html:
        with open(args.html, 'w') as f:
            f.write('<!doctype html><meta charset="utf-8"><title>Texture comparison</title><table border=1>'
                    '<tr><th>texture<th>result<th>max<th>mean<th>PSNR<th>ours<th>theirs</tr>')
            for stem, r, other in rows:
                ours = os.path.abspath(os.path.join(args.ours, stem + '.png'))
                theirs_img = '<img src="file:///%s" width=128>' % os.path.abspath(other) if other else ''
                f.write('<tr><td>%s<td>%s<td>%s<td>%s<td>%s<td><img src="file:///%s" width=128><td>%s</tr>' %
                        (stem, 'ok' if r['ok'] else 'FAIL: ' + r['why'], r.get('max', ''), r.get('mean', ''),
                         '%.1f' % r['psnr'] if 'psnr' in r else '', ours, theirs_img))
            f.write('</table>')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
