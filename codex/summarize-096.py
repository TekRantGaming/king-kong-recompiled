"""Offline summaries and dependency-free SVG plots of task 096's CSV evidence."""
import argparse
import csv
import html
import json
import math
from pathlib import Path
import statistics


def summarize(values):
    values = [v for v in values if math.isfinite(v) and v > 0]
    if not values:
        return {"samples": 0}
    ordered = sorted(values)
    count = len(values)
    low_count = math.ceil(count / 100)
    p50 = ordered[math.ceil(count * .50) - 1]
    p99 = ordered[math.ceil(count * .99) - 1]
    return dict(samples=count, seconds=sum(values) / 1000,
                average_fps=1000 * count / sum(values),
                low_fps=1000 * low_count / sum(ordered[-low_count:]),
                worst_ms=max(values), stdev_ms=statistics.pstdev(values),
                p50_ms=p50, p99_ms=p99, p99_p50_ms=p99 - p50)


def plot(path, panels):
    width, panel_height = 1100, 260
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{panel_height * len(panels)}" viewBox="0 0 {width} {panel_height * len(panels)}">',
             '<rect width="100%" height="100%" fill="white"/>',
             '<style>text{font:14px sans-serif;fill:#202934}.axis{stroke:#66717e;stroke-width:1}.grid{stroke:#dde3e9;stroke-width:1}</style>']
    for panel, (title, series) in enumerate(panels):
        y0, y1 = panel * panel_height + 46, panel * panel_height + 207
        x0, x1 = 76, width - 24
        maximum = max((max(v) for _, v in series if v), default=1) * 1.05
        seconds = max((sum(v) / 1000 for _, v in series), default=1)
        parts.append(f'<text x="{x0}" y="{y0-25}">{html.escape(title)}</text>')
        for tick in range(5):
            value = maximum * tick / 4
            y = y1 - (y1 - y0) * tick / 4
            parts += [f'<path class="grid" d="M{x0},{y} H{x1}"/>',
                      f'<text x="{x0-8}" y="{y+5}" text-anchor="end">{value:.1f}</text>']
        for tick in range(6):
            x = x0 + (x1 - x0) * tick / 5
            parts.append(f'<text x="{x}" y="{y1+22}" text-anchor="middle">{seconds*tick/5:.1f}</text>')
        parts += [f'<text x="12" y="{y0+20}">ms</text>',
                  f'<text x="{x1}" y="{y1+44}" text-anchor="end">elapsed seconds</text>']
        for index, (label, values) in enumerate(series):
            color = ["#c25429", "#176eae", "#328139", "#8753a4"][index % 4]
            elapsed = 0
            # Preserve both min/max in each pixel bucket, including isolated hitches.
            buckets = {}
            for value in values:
                x = int(x0 + (x1 - x0) * elapsed / seconds)
                if x not in buckets:
                    buckets[x] = [value, value]
                else:
                    buckets[x][0] = min(buckets[x][0], value)
                    buckets[x][1] = max(buckets[x][1], value)
                elapsed += value / 1000
            points = []
            for x, (minimum, peak) in buckets.items():
                for value in (minimum, peak):
                    points.append(f"{x},{y1-(y1-y0)*value/maximum:.2f}")
            parts.append(f'<polyline points="{" ".join(points)}" fill="none" stroke="{color}" stroke-width="1" opacity=".75"/>')
            lx = x0 + index * 265
            parts.append(f'<text x="{lx}" y="{y0-6}" style="fill:{color}">{html.escape(label)}</text>')
        parts.append(f'<path class="axis" fill="none" d="M{x0},{y0} V{y1} H{x1}"/>')
    parts.append('</svg>')
    path.write_text("\n".join(parts), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runs', type=Path)
    parser.add_argument('--synthetic', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    summary = {}
    if args.runs:
        runs = json.loads(args.runs.read_text(encoding='utf-8-sig'))
        panels = []
        data = {}
        for run in runs:
            root = Path(run['run'])
            results = list((root / 'userdata' / 'benchmarks').glob('*/frames.csv'))
            if len(results) != 1:
                summary[run['label']] = {'error': f'{len(results)} result CSVs'}
                continue
            series = {'game': [], 'host': []}
            with results[0].open(newline='') as f:
                for row in csv.DictReader(f):
                    series[row['series']].append(float(row['frame_ms']))
            data[run['label']] = series
            summary[run['label']] = {k: summarize(v) for k, v in series.items()}
            panels.extend((f"{run['label']}: {name} intervals", [(name, values)]) for name, values in series.items())
        plot(args.output / 'game-runs.svg', panels)
        if 'before-mailbox' in data and 'after-mailbox' in data:
            plot(args.output / 'before-after.svg', [
                (f"Vulkan mailbox / cap 60: {name} intervals (host = successful returns, not scanout)",
                 [(label, data[label][name]) for label in ('before-mailbox', 'after-mailbox')])
                for name in ('game', 'host')])
    if args.synthetic:
        groups = {}
        with args.synthetic.open(newline='') as f:
            for row in csv.DictReader(f):
                key = (row['limiter'], row['fps'], row['stalled'])
                groups.setdefault(key, []).append(float(row['frame_ms']))
        summary['synthetic'] = {','.join(k): summarize(v) for k, v in groups.items()}
        panels = []
        for stalled in ('0', '1'):
            for fps in ('30', '60', '120', '144', '240'):
                panels.append((f"Offline limiter / cap {fps} / injected work overrun {stalled}",
                               [(label, groups[(label, fps, stalled)]) for label in ('before', 'after')]))
        plot(args.output / 'synthetic.svg', panels)
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
