"""Score one private chapter/checkpoint scenario, retaining every log's identity."""
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import sys
import time

spec = importlib.util.spec_from_file_location("world_diff", Path(__file__).with_name("world-diff.py"))
w = importlib.util.module_from_spec(spec)
spec.loader.exec_module(w)


def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024*1024), b""):
            h.update(block)
    return h.hexdigest()


def score(evidence_dir, run):
    evidence = w.read(evidence_dir / "evidence.json")
    result = w.series(evidence_dir / "host", evidence_dir / "guest")
    # Capture and analysis can be revised independently. Retain exact analyzer
    # sources/hashes per scoring pass, in addition to the launch manifest.
    analysis_dir = evidence_dir / ("analysis-tools-" + str(time.time_ns()))
    analysis_dir.mkdir()
    result["analysis_harness"] = {}
    for tool_name in ("world-diff.py", "world-sweep-score.py"):
        source = Path(__file__).with_name(tool_name)
        target = analysis_dir / tool_name
        target.write_bytes(source.read_bytes())
        result["analysis_harness"][tool_name] = {"sha256": sha(target), "source": str(target)}
    result.update(entry=evidence["entry"], requested_start=evidence["start"], load_proof=evidence["load_proof"],
                  campaign_progression="UNPROVEN: forced entry/checkpoint smoke only", objects="PARTIAL: native bound families only",
                  artifacts=[], actions={})
    result["policy"] = evidence["policy"]
    result["native_capture_status"] = result["status"]
    ids = {}
    logs_ok = True
    for role, log_role in (("host", "host"), ("guest", "join")):
        paths = list((evidence_dir / role).glob("world-g*.json"))
        dumps = [w.read(p) for p in paths]
        ids[role] = sorted({i for d in dumps if d.get("identity", {}).get("level") == evidence["level"]
                            and not any("native-level-mismatch" in e or "native-load-not-ready" in e or "checkpoint-" in e for e in d.get("errors", []))
                            for i in d.get("native_checkpoint_ids", [])})
        players = [d["local"]["player"] for d in dumps if d.get("local", {}).get("player")]
        movement = max((math.dist(a["position"], b["position"]) for a in players for b in players), default=0)
        shots = max((p.get("fire", 0) for p in players), default=0)
        log_paths = sorted((run / log_role).glob("run*.log"))
        fatal, loaded, simulated, shot_applied, story = [], False, False, 0, 0
        digest = dict(samples=0, quiet=0, moves=0, guest_reloads=0, decisions=[])
        chaos_counters = []
        for path in log_paths:
            result["artifacts"].append({"path": str(path), "sha256": sha(path), "bytes": path.stat().st_size})
            with path.open(encoding="utf-8-sig", errors="replace") as f:
                for n, line in enumerate(f, 1):
                    if re.search(r"\[(?:critical|fatal)\]|unhandled exception|assertion failed|fatal error|VK_ERROR_DEVICE_LOST", line, re.I):
                        fatal.append({"path": str(path), "line": n, "text": line.strip()})
                    loaded |= "KK co-op flow: loaded generation" in line
                    simulated |= "KK dev UDP chaos:" in line and "profile=bad_wifi" in line and "seed=86" in line
                    match = re.search(r"KK co-op desync: samples=(\d+).*quiet=(\d+) moves=(\d+) guest-reloads=(\d+)", line)
                    if match:
                        for key, value in zip(("samples", "quiet", "moves", "guest_reloads"), match.groups()):
                            digest[key] = max(digest[key], int(value))
                    if "KK co-op desync: decision=" in line or "KK co-op desync: completed=" in line:
                        digest["decisions"].append(line.strip())
                    if "chaos" in line.lower() and "drop" in line.lower():
                        chaos_counters.append(line.strip())
                    shot_applied += "ENEMIES shot number=" in line and "native-queued" in line
                    story += "KK co-op story:" in line and any(s in line for s in ("native hint recorded", "applied", "native loop"))
        result["actions"][role] = {"native_walk_displacement": movement, "walk": movement > .5,
                                   "native_confirmed_shots": shots, "fire": shots > 0,
                                   "native_shot_queues": shot_applied, "hit": "UNPROVEN: queue alone is not native damage",
                                   "story_events": story, "trigger": "UNPROVEN: no scenario-specific trigger postcondition",
                                   "load_logged": loaded, "wan_logged": simulated, "log_files": len(log_paths), "fatal": fatal,
                                   "digest": digest, "chaos_counters": chaos_counters}
        for path in paths:
            result["artifacts"].append({"path": str(path), "sha256": sha(path), "bytes": path.stat().st_size})
        logs_ok &= bool(log_paths) and loaded and simulated and not fatal
    result["native_checkpoint_ids"] = sorted(set(ids["host"]) & set(ids["guest"]))
    result["checkpoint_ids_by_peer"] = ids
    # The approved runner must delete all private binaries and caches after a run.
    # Its copy-time hash survives cleanup and is separate from the source build hash.
    binary_audit = run / "binary-audit.json"
    result["actual_exe"] = str(binary_audit)
    result["actual_exe_sha256"] = w.read(binary_audit).get("executable_sha256") if binary_audit.exists() else None
    if result["actual_exe_sha256"] != evidence["exe_sha256"] or not logs_ok:
        result["errors"].append("private executable/log/load/WAN evidence incomplete or mismatched")
        result["status"] = "INVALID"
    elif evidence["policy"] != "Together":
        result["status"] = "UNSUPPORTED_POLICY"
        result["policy_comparison"] = "Independent worlds allowed; shared equality not applicable. Native capture limits remain recorded."
    result["scenario_acceptance"] = "UNPROVEN: missing hit/trigger/object postconditions"
    previous = evidence_dir / "result.json"
    if previous.exists():
        retained = evidence_dir / "previous-results"
        retained.mkdir(exist_ok=True)
        previous.rename(retained / ("result-" + str(time.time_ns()) + ".json"))
    (evidence_dir / "result.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(result["status"], "entry=" + str(evidence["entry"]), "start=" + str(evidence["start"]),
          "diffs=" + str(len(result["differences"])), "checkpoints=" + str(result["native_checkpoint_ids"]))
    return result


if __name__ == "__main__":
    try:
        score(Path(sys.argv[1]), Path(sys.argv[2]))
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print("INVALID scorer:", exc, file=sys.stderr)
        sys.exit(2)
