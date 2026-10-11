"""Compare game-state samples (KK_DEV_SAMPLE, kk/src/dev_sample.cpp) from runs at different frame rates.

A system is frame-rate dependent when its value over game time differs from the 30 FPS run beyond the
noise between two 30 FPS runs. Runs are lined up on the level's own clock (gt, the sum of frame times
since the world was loaded); objects are matched by their flags and their position at the first common
moment.

    python -I tools/hfr_compare.py summary run30/sample.csv
    python -I tools/hfr_compare.py compare run30/sample.csv run60/sample.csv [--top 25] [--from 5 --to 60]
    python -I tools/hfr_compare.py track run30/sample.csv run60/sample.csv --obj 12 [--field ev_t]

summary: frame pacing (frames per second, dt spread) and the objects seen.
compare: per matched object, the RMS difference of position over game time, the speed of its first event
         track (event-time seconds per game second; 1.0 = real time) and how often the track restarts,
         and how often its first AI track changes function. Biggest differences first.
track:   one object's values side by side over game time (for plots or eyeballing).
"""

import argparse
import bisect
import csv
import math
import sys
from collections import defaultdict


def load(path):
    frames, objs = [], defaultdict(list)
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            row = {k: (float(v) if k not in ('obj', 'flags', 'ai_f', 'key') else v) for k, v in r.items()}
            if int(row['i']) < 0:
                frames.append(row)
            else:
                key = row.get('key', '00000000')
                objs[key if key and key != '00000000' else '@' + row['obj']].append(row)
    # Only the last world's samples (the level being compared); gt restarts when a world loads.
    if frames:
        world = frames[-1]['obj']
        start = max(i for i, fr in enumerate(frames) if i == 0 or frames[i - 1]['obj'] != fr['obj'])
        t0 = frames[start]['t']
        frames = frames[start:]
        objs = {k: [r for r in v if r['t'] >= t0] for k, v in objs.items()}
        objs = {k: v for k, v in objs.items() if v}
    return frames, objs


def summary(path):
    frames, objs = load(path)
    if not frames:
        print('no frames')
        return
    dts = sorted(fr['dt'] for fr in frames)
    span = frames[-1]['t'] - frames[0]['t']
    print(f'{path}: world {frames[-1]["obj"]}, {len(frames)} frames over {span:.1f} s = {len(frames) / max(span, 1e-9):.1f} FPS;'
          f' dt median {dts[len(dts) // 2] * 1000:.2f} ms, 1% {dts[len(dts) // 100] * 1000:.2f}, 99% {dts[-len(dts) // 100 - 1] * 1000:.2f};'
          f' game time {frames[-1]["gt"]:.1f} s; {len(objs)} objects')


_gts = {}


def interp(rows, key, gt):
    xs = _gts.get(id(rows))
    if xs is None:
        xs = _gts[id(rows)] = [r['gt'] for r in rows]
    i = bisect.bisect_left(xs, gt)
    if i <= 0:
        return rows[0][key]
    if i >= len(rows):
        return rows[-1][key]
    a, b = rows[i - 1], rows[i]
    if b['gt'] == a['gt']:
        return a[key]
    w = (gt - a['gt']) / (b['gt'] - a['gt'])
    return a[key] + (b[key] - a[key]) * w


def match(objs_a, objs_b, at):
    """Pairs objects of run A with run B: the same file key, else same flags and nearest position at game
    time `at`."""
    pos = lambda rows: (interp(rows, 'x', at), interp(rows, 'y', at), interp(rows, 'z', at))
    pairs = [(k, k, 0.0) for k in objs_a if not k.startswith('@') and k in objs_b]
    objs_a = {k: v for k, v in objs_a.items() if k.startswith('@')}
    objs_b = {k: v for k, v in objs_b.items() if k.startswith('@')}
    cand = []
    for ka, ra in objs_a.items():
        pa = pos(ra)
        for kb, rb in objs_b.items():
            if ra[0]['flags'] != rb[0]['flags']:
                continue
            pb = pos(rb)
            cand.append((math.dist(pa, pb), ka, kb))
    cand.sort()
    used_a, used_b = set(), set()
    for d, ka, kb in cand:
        if ka in used_a or kb in used_b or d > 1.0:
            continue
        used_a.add(ka), used_b.add(kb)
        pairs.append((ka, kb, d))
    return pairs


def track_stats(rows, lo, hi):
    """Event track speed (event seconds per game second while it runs) and restarts per game second;
    AI function changes per game second."""
    rows = [r for r in rows if lo <= r['gt'] <= hi]
    if len(rows) < 3:
        return None
    adv, game, restarts, ai_changes = 0.0, 0.0, 0, 0
    for a, b in zip(rows, rows[1:]):
        dg = b['gt'] - a['gt']
        de = b['ev_t'] - a['ev_t']
        if b['ev_i'] < a['ev_i'] or de < -1e-4:
            restarts += 1
        elif de > 0:
            adv += de
            game += dg
        if b['ai_f'] != a['ai_f']:
            ai_changes += 1
    span = rows[-1]['gt'] - rows[0]['gt']
    return (adv / game if game > 0 else 0.0, restarts / span if span > 0 else 0.0,
            ai_changes / span if span > 0 else 0.0)


def compare(pa, pb, top, lo, hi):
    fa, oa = load(pa)
    fb, ob = load(pb)
    summary(pa)
    summary(pb)
    end = min(fa[-1]['gt'], fb[-1]['gt'])
    lo = max(lo, 0.5)
    hi = min(hi, end)
    pairs = match(oa, ob, lo)
    print(f'{len(pairs)} objects matched of {len(oa)} / {len(ob)}; game time {lo:.1f} to {hi:.1f} s')
    rows = []
    for ka, kb, d0 in pairs:
        ra, rb = oa[ka], ob[kb]
        n, s = 0, 0.0
        g = lo
        while g <= hi:
            s += sum((interp(ra, k, g) - interp(rb, k, g)) ** 2 for k in ('x', 'y', 'z'))
            n += 1
            g += 0.1
        rms = math.sqrt(s / max(n, 1))
        sa, sb = track_stats(ra, lo, hi), track_stats(rb, lo, hi)
        moved = max(math.dist((r['x'], r['y'], r['z']), (ra[0]['x'], ra[0]['y'], ra[0]['z'])) for r in ra)
        rows.append((rms, ka, kb, moved, sa, sb))
    rows.sort(key=lambda r: -r[0])
    print(f'{"pos rms":>8} {"moved":>7}  {"obj A":8} {"obj B":8}  ev speed A/B   restarts/s A/B   ai changes/s A/B')
    for rms, ka, kb, moved, sa, sb in rows[:top]:
        ev = f'{sa[0]:5.2f} {sb[0]:5.2f}' if sa and sb else '    -'
        rs = f'{sa[1]:6.2f} {sb[1]:6.2f}' if sa and sb else '    -'
        ai = f'{sa[2]:6.2f} {sb[2]:6.2f}' if sa and sb else '    -'
        print(f'{rms:8.3f} {moved:7.2f}  {ka} {kb}  {ev}   {rs}   {ai}')
    # Event tracks whose speed or restart rate differ, whatever the position does.
    print('\nevent / AI rates that differ by more than 15% (or 0.2 a second):')
    for rms, ka, kb, moved, sa, sb in sorted(rows, key=lambda r: r[1]):
        if not (sa and sb):
            continue
        diffs = []
        for name, i in (('ev speed', 0), ('restarts/s', 1), ('ai changes/s', 2)):
            x, y = sa[i], sb[i]
            if abs(x - y) > max(0.2, 0.15 * max(abs(x), abs(y))):
                diffs.append(f'{name} {x:.2f} -> {y:.2f}')
        if diffs:
            print(f'  {ka} {kb}: ' + ', '.join(diffs))


def track(pa, pb, obj, field):
    fa, oa = load(pa)
    fb, ob = load(pb)
    keys = sorted(oa)
    ka = keys[int(obj)] if obj.isdigit() and len(obj) < 6 else obj
    pairs = {a: b for a, b, _ in match(oa, ob, 1.0)}
    kb = pairs.get(ka)
    print(f'gt,{field}_A,{field}_B   ({ka} -> {kb})')
    for r in oa[ka]:
        b = interp(ob[kb], field, r['gt']) if kb else float('nan')
        print(f'{r["gt"]:.3f},{r[field]:.4f},{b:.4f}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('summary')
    s.add_argument('csv', nargs='+')
    c = sub.add_parser('compare')
    c.add_argument('a')
    c.add_argument('b')
    c.add_argument('--top', type=int, default=25)
    c.add_argument('--from', dest='lo', type=float, default=0.0)
    c.add_argument('--to', dest='hi', type=float, default=1e9)
    t = sub.add_parser('track')
    t.add_argument('a')
    t.add_argument('b')
    t.add_argument('--obj', required=True)
    t.add_argument('--field', default='x')
    a = ap.parse_args()
    if a.cmd == 'summary':
        for p in a.csv:
            summary(p)
    elif a.cmd == 'compare':
        compare(a.a, a.b, a.top, a.lo, a.hi)
    else:
        track(a.a, a.b, a.obj, a.field)


if __name__ == '__main__':
    sys.exit(main())
