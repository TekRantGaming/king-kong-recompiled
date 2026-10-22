"""Score one private chapter/checkpoint scenario, retaining every log's identity."""
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import sys

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
    result.update(entry=evidence["entry"], requested_start=evidence["start"], load_proof=evidence["load_proof"],
                  campaign_progression="UNPROVEN: forced entry/checkpoint smoke only", objects="UNSUPPORTED: C71 not integrated",
                  artifacts=[], actions={})
    ids = {}
    logs_ok = True
    for role, log_role in (("host", "host"), ("guest", "join")):
        paths = list((evidence_dir / role).glob("world-g*.json"))
        dumps = [w.read(p) for p in paths]
        ids[role] = sorted({i for d in dumps if d.get("identity", {}).get("level") == evidence["level"] for i in d.get("native_checkpoint_ids", [])})
        players = [d["local"]["player"] for d in dumps if d.get("local", {}).get("player")]
        movement = max((math.dist(a["position"], b["position"]) for a in players for b in players), default=0)
        shots = max((p.get("fire", 0) for p in players), default=0)
        log_paths = sorted((run / log_role).glob("run*.log"))
        fatal, loaded, simulated, shot_applied, story = [], False, False, 0, 0
        for path in log_paths:
            result["artifacts"].append({"path": str(path), "sha256": sha(path), "bytes": path.stat().st_size})
            with path.open(encoding="utf-8-sig", errors="replace") as f:
                for n, line in enumerate(f, 1):
                    if re.search(r"\[(?:critical|fatal)\]|unhandled exception|assertion failed|fatal error", line, re.I):
                        fatal.append({"path": str(path), "line": n, "text": line.strip()})
                    loaded |= "KK co-op flow: loaded generation" in line
                    simulated |= "KK dev UDP simulation:" in line and "one-way-ms=150" in line and "loss-percent=3" in line
                    shot_applied += "ENEMIES shot number=" in line and "native-queued" in line
                    story += "KK co-op story:" in line and any(s in line for s in ("native hint recorded", "applied", "native loop"))
        result["actions"][role] = {"native_walk_displacement": movement, "walk": movement > .5,
                                   "native_confirmed_shots": shots, "fire": shots > 0,
                                   "native_shot_queues": shot_applied, "hit": "UNPROVEN: queue alone is not native damage",
                                   "story_events": story, "trigger": "UNPROVEN: no scenario-specific trigger postcondition",
                                   "load_logged": loaded, "wan_logged": simulated, "log_files": len(log_paths), "fatal": fatal}
        for path in paths:
            result["artifacts"].append({"path": str(path), "sha256": sha(path), "bytes": path.stat().st_size})
        logs_ok &= bool(log_paths) and loaded and simulated and not fatal
    result["native_checkpoint_ids"] = sorted(set(ids["host"]) & set(ids["guest"]))
    result["checkpoint_ids_by_peer"] = ids
    binary = run / "bin" / "king_kong.exe"
    if not binary.exists():
        binary = Path("F:/kk-native-coop/moved-aside") / run.name / "bin" / "king_kong.exe"
    result["actual_exe"] = str(binary)
    result["actual_exe_sha256"] = sha(binary) if binary.exists() else None
    if result["actual_exe_sha256"] != evidence["exe_sha256"] or not logs_ok:
        result["errors"].append("private executable/log/load/WAN evidence incomplete or mismatched")
        result["status"] = "INVALID"
    result["scenario_acceptance"] = "UNPROVEN: missing hit/trigger/object postconditions"
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
