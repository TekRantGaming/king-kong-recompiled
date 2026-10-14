# Does each graphics panel setting reach the picture? Compares the captures of a verify run
# (tools/graphics-panel-run.ps1 -Verify, KK_DEV_PANEL_VERIFY): for every setting and each of its other values,
# three trials of its own value (A), the value (V) and its own value again (B), 0.8 s apart. A against B is what
# the scene changes by itself in that time (the noise), the smaller of V against A or B the setting's change;
# the median of the three trials drops a lightning flash.
#   graphics-panel-verify.py <run folder> [--md]
import glob, os, re, sys

import numpy as np
from PIL import Image


def load(path):
    img = Image.open(path).convert('RGB')
    w = 1920
    return np.asarray(img.resize((w, int(img.height * w / img.width)), Image.BILINEAR), dtype=np.int16)


def dilate(mask, r=2):
    out = mask.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            out |= np.roll(np.roll(mask, dy, axis=0), dx, axis=1)
    return out


def detail(img):
    g = img.mean(axis=2)
    return np.abs(np.diff(g, axis=0))[:, :-1] + np.abs(np.diff(g, axis=1))[:-1, :]


def trial(A, V, B):
    """The setting's change on the pixels the scene kept still between A and B (rain, fire and flicker left
    out): the share that changed, the mean change there, and the picture detail of V against A and B."""
    unstable = dilate(np.abs(A - B).max(axis=2) > 12)
    stable = ~unstable
    coverage = float(stable.mean())
    if coverage < 0.2:
        return None
    ref = (A + B) / 2.0
    d = np.abs(V - ref).max(axis=2)
    share = float((d[stable] > 8).mean() * 100.0)
    mean = float(d[stable].mean())
    st = stable[:-1, :-1]
    da, dv = detail(ref)[st].mean(), detail(V)[st].mean()
    return share, mean, float(dv / da) if da > 0 else 1.0, coverage * 100.0


def main(run, md):
    caps = os.path.join(run, 'captures')
    log = open(os.path.join(run, 'run.log'), encoding='utf-8', errors='replace').read()
    applied = {m.group(1): m.group(2) for m in re.finditer(r'KK panel test: verify (\S+) base=\S* upscaler=.*? applied=(.*?) gpu', log)}
    rows = []
    groups = sorted({f[:-len('-T0-A.png')] for f in glob.glob(os.path.join(caps, '*-T0-A.png'))},
                    key=lambda p: (int(os.path.basename(p).split('-')[0]), p))
    for g in groups:
        name = os.path.basename(g)
        m = re.match(r'(\d+)-(.+)-V(\d+)-(.+)$', name)
        step, cvar, value = int(m.group(1)), m.group(2), m.group(4)
        trials, nulls = [], []
        for t in range(3):
            fa, fv, fb = ('%s-T%d-%s.png' % (g, t, w) for w in 'AVB')
            if not all(os.path.exists(f) for f in (fa, fv, fb)):
                continue
            A, B = load(fa), load(fb)
            r = trial(A, load(fv), B)
            if r:
                trials.append(r)
            # The null test: the next trial's own-value frame in V's place (what the scene alone changes).
            nxt = '%s-T%d-A.png' % (g, t + 1)
            if os.path.exists(nxt):
                n = trial(A, load(nxt), B)
                if n:
                    nulls.append(n)
        if not trials:
            rows.append((step, cvar, value, None, None, 'no still pixels', applied.get(name, '')))
            continue
        share = float(np.median([t[0] for t in trials]))
        mean = float(np.median([t[1] for t in trials]))
        sharp = float(np.median([t[2] for t in trials]))
        null_share = float(np.median([n[0] for n in nulls])) if nulls else 0.0
        null_sharp = float(np.median([abs(n[2] - 1.0) for n in nulls])) if nulls else 0.0
        changed = 'changed' if (share > max(3.0 * null_share, null_share + 1.0) or
                                abs(sharp - 1.0) > max(3.0 * null_sharp, 0.04)) else 'UNCHANGED'
        noise = (null_share, null_sharp)
        d = (share, mean, sharp)
        rows.append((step, cvar, value, noise, d, changed, applied.get(name, '')))
    if md:
        print('| # | setting | value | scene alone: changed % / detail | setting: changed % / detail | result | renderer applied |')
        print('|---|---|---|---|---|---|---|---|---|')
    for step, cvar, value, noise, d, changed, app in rows:
        if d is None:
            print(('| %d | %s | %s | - | - | - | - | %s | %s |' if md else '%3d %-28s %-18s %s %s') % (step, cvar, value, changed, app))
            continue
        if md:
            print('| %d | %s | %s | %.2f / %.3f | %.2f / %.3f | %s | %s |' % (step, cvar, value, noise[0], 1 + noise[1], d[0], d[2], changed, app))
        else:
            print('%3d %-28s %-18s null %5.2f%% %.3f  changed %6.2f%%  detail %.3f  %-9s %s' %
                  (step, cvar, value, noise[0], noise[1], d[0], d[2], changed, app))


if __name__ == '__main__':
    main(sys.argv[1], '--md' in sys.argv)
