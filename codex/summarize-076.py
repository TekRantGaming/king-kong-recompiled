"""Summarize task076 evidence; counts are successful host presents, not unique images."""
from pathlib import Path
import datetime as dt
import json
import re
import statistics
import sys

workspace = Path(__file__).resolve().parent.parent
out = workspace / "out/task076"
bench = {}
for side in ("before", "after"):
    rows = {}
    for repeat in range(1, 4):
        text = (out / f"bench-{side}-{repeat}.log").read_text()
        if "1 tests, 0 failed checks" not in text:
            raise RuntimeError(f"Incomplete benchmark {side} {repeat}")
        for match in re.finditer(r"BENCH (.*?)\s+801 draws: median\s+[\d.]+ us/frame \(([\d.]+) us/draw\)", text):
            rows.setdefault(match[1].strip(), []).append(float(match[2]))
    bench[side] = {name: {"repeats_us_per_draw": values, "median_us_per_draw": statistics.median(values)} for name, values in rows.items()}
result = {"bench": bench}
if len(sys.argv) > 1:
    campaign = Path(sys.argv[1])
    game = {}
    for api in ("vulkan", "d3d12"):
        lines = (campaign / api / "run.log").read_text(encoding="utf-8").splitlines()
        first = next(line for line in lines if "first game frame shown" in line)
        timestamp = lambda line: dt.datetime.fromisoformat(line[1:24])
        origin = timestamp(first)
        guest = []
        hosts = []
        # Startup/pipeline warmup excluded; fixed 15-35 s selection per API.
        for line in lines:
            try:
                end = (timestamp(line) - origin).total_seconds()
            except ValueError:
                continue
            match = re.search(r"UI paint trace: (\d+) host presents in ([\d.]+) s", line)
            if match and end - float(match[2]) >= 15 and end <= 35:
                hosts.append((int(match[1]), float(match[2])))
            match = re.search(r"Guest FPS: ([\d.]+)", line)
            if match and 16 <= end <= 35:
                guest.append(float(match[1]))
        count = sum(row[0] for row in hosts)
        seconds = sum(row[1] for row in hosts)
        game[api] = {"window_seconds_after_first_frame": [15, 35], "host_count": count,
                     "host_seconds": seconds, "host_per_second": count / seconds if seconds else None,
                     "guest_fps_median": statistics.median(guest) if guest else None,
                     "guest_fps_range": [min(guest), max(guest)] if guest else None,
                     "guest_samples": len(guest)}
    result["game"] = game
print(json.dumps(result, indent=2))
