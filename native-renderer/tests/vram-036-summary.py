"""Summarise preserved task 036 telemetry; never infer results for queued runs."""
import csv
import datetime
import json
import re
import statistics
from pathlib import Path

ROOT = Path(r"F:\KK-native-renderer\runs")
OUT = ROOT / "task036-analysis"
OUT.mkdir(exist_ok=True)
curves, summaries = [], []
for run in sorted(ROOT.glob("task036-*-*-*-20*")):
    if not (run / "test.json").exists():
        continue
    meta = json.loads((run / "test.json").read_text(encoding="utf-8-sig"))
    samples = {}
    texts = []
    for log in sorted(run.glob("run.log*")):
        text = log.read_text(encoding="utf-8", errors="replace")
        texts.append(text)
        for match in re.finditer(r"^\[([^\]]+)\].*memory budget frame (\d+): ([^\r\n]+)", text, re.M):
            sample = {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", match[3])}
            sample["frame"] = int(match[2])
            sample["timestamp"] = datetime.datetime.fromisoformat(match[1]).timestamp()
            samples[sample["frame"]] = sample
    ordered = sorted(samples.values(), key=lambda s: s["frame"])
    for sample in ordered:
        sample["seconds"] = round(sample["timestamp"] - ordered[0]["timestamp"], 3)
        curves.append({"run": run.name, **sample})
    intervals = []
    transition_intervals = []
    half_frame = next((s["frame"] for s in ordered if s["half_ao"]), None)
    trace = run / "render_frames.csv"
    if trace.exists() and trace.stat().st_size:
        rows = list(csv.DictReader(trace.open(encoding="utf-8-sig")))
        if rows:
            end = float(rows[-1]["wall_s"])
            intervals = [float(r["interval_ms"]) for r in rows if float(r["wall_s"]) >= end - 40]
            if half_frame is not None:
                transition_intervals = [float(r["interval_ms"]) for r in rows
                                        if half_frame - 2 <= int(r["frame"]) <= half_frame + 30]
    def percentile(values, p):
        if not values:
            return ""
        return round(sorted(values)[min(len(values) - 1, int((len(values) - 1) * p))], 3)
    logs = "\n".join(texts)
    summary = {
        "run": run.name, "control": meta["control"], "budget_gib": meta["budget_gib"], "case": meta["case"],
        "simulated_pressure": meta.get("pressure", False), "gtao": meta.get("gtao", False),
        "pressure_delay_s": meta.get("pressure_delay_s", 0), "dll_sha256": meta["dll_sha256"],
        "tour_kind": meta.get("tour_kind", "legacy-menu" if meta["case"] == "tour" else ""),
        "samples": len(ordered), "pressure_samples": sum(s["pressure"] for s in ordered),
        "usage_start_mib": round(ordered[0]["usage"] / 2**20, 2) if ordered else "",
        "usage_peak_mib": round(max(s["usage"] for s in ordered) / 2**20, 2) if ordered else "",
        "usage_end_mib": round(ordered[-1]["usage"] / 2**20, 2) if ordered else "",
        "target_peak_mib": round(max(s["target_bytes"] for s in ordered) / 2**20, 2) if ordered else "",
        "evictions": max((s["evictions"] for s in ordered), default=0),
        "restores": max((s["restores"] for s in ordered), default=0),
        "half_ao": max((s["half_ao"] for s in ordered), default=0),
        "late_frame_samples": len(intervals), "late_p50_ms": percentile(intervals, .5),
        "late_p95_ms": percentile(intervals, .95), "late_p99_ms": percentile(intervals, .99),
        "half_ao_frame": half_frame if half_frame is not None else "",
        "ao_transition_worst_ms": round(max(transition_intervals), 3) if transition_intervals else "",
        "tour_transition_requested": "asking for the next chapter's level (entry 9)" in logs,
        "captures": len(list(run.glob("shot*.bmp"))),
        "error_lines": sum(bool(re.search(r"\b(error|fatal|VUID-|device removed|device lost|exhausted)\b", line, re.I))
                           for line in logs.splitlines()),
    }
    summaries.append(summary)
for name, rows in [("curves.csv", curves), ("summary.csv", summaries)]:
    if rows:
        fields = list(dict.fromkeys(key for row in rows for key in row))
        with (OUT / name).open("w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)
print(json.dumps(summaries, indent=2))
