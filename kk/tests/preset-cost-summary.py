"""Summarise task 338's production renderer timing windows within settled phases.

Run with the Python312 absolute path and -I; evidence stays beside the game run.
Windows steady_clock and perf_counter share the system QPC clock.
"""
import csv
import datetime
import json
import math
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time

root = Path(sys.argv[1])
# Additional roots are analysed independently before combining their phase means.
for extra in sys.argv[2:]:
    subprocess.run([sys.executable, "-I", __file__, extra], check=True, capture_output=True, text=True)
offset = time.time() - time.perf_counter()
phases = list(csv.DictReader((root / "captures/phases.csv").open()))
pattern = re.compile(
    r"^\[(?P<time>[^]]+)\].*frame timing \(\d+ frames, (?P<seconds>[\d.]+) s,.*?"
    r"game thread CPU (?P<game>[\d.-]+) ms,.*?render thread CPU (?P<render>[\d.]+) ms,.*?"
    r"GPU frame (?P<gpu>[\d.-]+) ms .*?GPU passes ms: AO (?P<ao>[\d.-]+), "
    r"GI (?P<gi>[\d.-]+), upscaler (?P<upscaler>[\d.-]+), shadows (?P<shadows>[\d.-]+), "
    r"water (?P<water>[\d.-]+), volumetrics (?P<shafts>[\d.-]+)"
)
metrics = ("game", "render", "gpu", "ao", "gi", "upscaler", "shadows", "water", "shafts")
windows = []
for line in (root / "run.log").read_text(errors="replace").splitlines():
    m = pattern.search(line)
    if not m:
        continue
    end = datetime.datetime.fromisoformat(m["time"]).timestamp() - offset
    windows.append((end - float(m["seconds"]), end,
                    {k: float(m[k]) if m[k] != "-" else math.nan for k in metrics}))
rows = []
for p in phases:
    start, end = float(p["wall_from"]), float(p["wall_to"])
    selected = [w[2] for w in windows if w[0] >= start + 0.06 and w[1] <= end - 0.06]
    row = dict(name=p["name"], windows=len(selected), input=p["input"], output=p["output"],
               upscaler=p["upscaler"], api=p["api"])
    for k in metrics:
        values = [w[k] for w in selected if math.isfinite(w[k])]
        row[k] = statistics.mean(values) if values else None
    rows.append(row)
with (root / "timings.csv").open("w", newline="") as f:
    writer = csv.DictWriter(f, fieldnames=rows[0].keys())
    writer.writeheader()
    writer.writerows(rows)
for extra in sys.argv[2:]:
    for row in csv.DictReader((Path(extra) / "timings.csv").open()):
        row["windows"] = int(row["windows"])
        for k in metrics:
            row[k] = float(row[k]) if row[k] else None
        rows.append(row)
groups = {}
for row in rows:
    name = re.sub(r"-\d+$", "", row["name"])
    groups.setdefault(name, []).append(row)
summary = {}
for name, group in groups.items():
    summary[name] = {"runs": len(group), "windows": [r["windows"] for r in group]}
    for k in metrics:
        values = [r[k] for r in group if r[k] is not None]
        summary[name][k] = dict(n=len(values), mean=statistics.mean(values), min=min(values), max=max(values)) if values else None
(root / ("summary-all.json" if len(sys.argv) > 2 else "summary.json")).write_text(json.dumps(summary, indent=2))
paired = {}
for row in rows:
    match = re.fullmatch(r"(.+)-([AB])-(\d+)", row["name"])
    if match:
        name, side, trial = match.groups()
        paired.setdefault(name, {}).setdefault(trial, {})[side] = row
savings = {}
for name, trials in paired.items():
    savings[name] = {}
    for k in metrics:
        values = [sides["A"][k] - sides["B"][k] for sides in trials.values()
                  if all(side in sides and sides[side][k] is not None for side in ("A", "B"))]
        savings[name][k] = dict(n=len(values), mean=statistics.mean(values), min=min(values), max=max(values)) if values else None
# Deck B and Low A write identical settings. Their separation measures drift
# from weather, clocks and other processes, without attributing it to a setting.
control = {}
for k in metrics:
    values = []
    for trial, sides in paired.get("deck", {}).items():
        other = paired.get("low", {}).get(trial, {})
        if "B" in sides and "A" in other and sides["B"][k] is not None and other["A"][k] is not None:
            values.append(abs(sides["B"][k] - other["A"][k]))
    control[k] = dict(n=len(values), mean=statistics.mean(values), min=min(values), max=max(values)) if values else None
savings["same_settings_drift"] = control
(root / ("savings-all.json" if len(sys.argv) > 2 else "savings.json")).write_text(json.dumps(savings, indent=2))
for name, data in summary.items():
    parts = [name, "windows=" + str(data["windows"])]
    for k in ("game", "render", "gpu"):
        v = data[k]
        parts.append(f"{k}={v['mean']:.3f} [{v['min']:.3f},{v['max']:.3f}]" if v else f"{k}=unavailable")
    print("; ".join(parts))
