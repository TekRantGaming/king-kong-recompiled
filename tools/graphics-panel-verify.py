# Does each graphics panel setting reach the picture? Compares the captures of a verify run
# (tools/graphics-panel-run.ps1 -Verify, KK_DEV_PANEL_VERIFY): for every setting, three captures of each other
# value (V<n>-<value>) between three of its own value before and after (B<n>, B<n+1>). The median of each
# three removes the rain and particles that move between captures; B<n> against B<n+1> is what the still scene
# changes by itself (the noise), the smaller of V against either the setting's change.
#   graphics-panel-verify.py <run folder> [--md]
import glob, os, re, sys

import numpy as np
from PIL import Image


def load(path):
    img = Image.open(path).convert('RGB')
    w = 960
    return np.asarray(img.resize((w, int(img.height * w / img.width)), Image.BILINEAR), dtype=np.int16)


def median(stem):
    files = sorted(glob.glob(stem + '-[0-9].png'))
    if not files:
        return None
    return np.median(np.stack([load(f) for f in files]), axis=0)


def diff(a, b):
    d = np.abs(a - b)
    return float(d.mean()), float((d.max(axis=2) > 6).mean() * 100.0)


def main(run, md):
    caps = os.path.join(run, 'captures')
    log = open(os.path.join(run, 'run.log'), encoding='utf-8', errors='replace').read()
    applied = {m.group(1): m.group(2) for m in re.finditer(r'KK panel test: verify (\S+) base=\S* upscaler=.*? applied=(.*?) gpu', log)}
    rows = []
    stems = sorted({f[:-len('-B0-0.png')] for f in glob.glob(os.path.join(caps, '*-B0-0.png'))},
                   key=lambda p: int(os.path.basename(p).split('-')[0]))
    for stem in stems:
        step, cvar = os.path.basename(stem).split('-', 1)
        k = 0
        while True:
            vs = glob.glob(stem + '-V%d-*-0.png' % k)
            if not vs:
                break
            value = vs[0][len(stem) + len('-V%d-' % k):-len('-0.png')]
            before, after = median(stem + '-B%d' % k), median(stem + '-B%d' % (k + 1))
            V = median(stem + '-V%d-%s' % (k, value))
            if before is None or V is None:
                break
            noise = diff(before, after) if after is not None else (0.0, 0.0)
            d1 = diff(V, before)
            d2 = diff(V, after) if after is not None else d1
            d = (min(d1[0], d2[0]), min(d1[1], d2[1]))
            changed = d[1] > max(2.0 * noise[1], 0.3) and d[0] > max(1.5 * noise[0], 0.05)
            rows.append((int(step), cvar, value, noise, d, changed,
                         applied.get(os.path.basename(stem) + '-V%d-%s' % (k, value), '')))
            k += 1
    if md:
        print('| # | setting | value | noise mean / % px | change mean / % px | result | renderer applied |')
        print('|---|---|---|---|---|---|---|')
    for step, cvar, value, noise, d, changed, app in rows:
        result = 'changed' if changed else 'UNCHANGED'
        if md:
            print('| %d | %s | %s | %.2f / %.1f | %.2f / %.1f | %s | %s |' % (step, cvar, value, noise[0], noise[1], d[0], d[1], result, app))
        else:
            print('%3d %-28s %-18s noise %5.2f %5.1f%%  change %6.2f %5.1f%%  %-9s %s' %
                  (step, cvar, value, noise[0], noise[1], d[0], d[1], result, app))


if __name__ == '__main__':
    main(sys.argv[1], '--md' in sys.argv)
