"""Retain every C76 settled mismatch in a private TSV, plus an honest coverage index."""
import csv
import json
from pathlib import Path
import sys

ROOT = Path("F:/kk-native-coop")


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def collect():
    scenarios, findings, manifests, seen = [], [], [], set()
    for path in sorted((ROOT / "runs").glob("chapter-sweep-*/manifest.json")):
        manifest = read(path)
        if manifest.get("source_branch") != "coop-c76":
            continue
        manifests.append(str(path))
        for record in manifest["runs"]:
            if record["status"] == "pending":
                continue
            result_path = Path(record["world_evidence"]) / "result.json"
            result = read(result_path) if result_path.exists() else {}
            scenarios.append({"entry": record["entry"], "start": record["start"], "run": record["run"],
                              "load_proof": record["load_proof"], "status": record["status"],
                              "world_status": result.get("status", record["world_status"]), "source_commit": manifest["source_commit"],
                              "result": str(result_path), "errors": result.get("errors", []), "native_errors": result.get("native_errors", {}),
                              "actions": result.get("actions", {}), "checkpoints": result.get("native_checkpoint_ids", []),
                              "settled_comparisons": result.get("settled_comparisons", 0), "final_status": result.get("final_status")})
            actual_start = result.get("identity", {}).get("start", record["start"])
            for diff in result.get("differences", []):
                host, guest = json.dumps(diff["host"], sort_keys=True), json.dumps(diff["guest"], sort_keys=True)
                identity = (record["entry"], actual_start, diff["key"], host, guest, manifest["source_commit"])
                if identity in seen:
                    continue
                seen.add(identity)
                findings.append({"entry": record["entry"], "checkpoint_start": actual_start, "key": diff["key"], "host": host, "guest": guest,
                                 "wrong_side": "UNKNOWN: no equivalent solo oracle", "source_commit": manifest["source_commit"],
                                 "run": record["run"], "evidence": str(result_path), "final_status": result.get("final_status", result.get("status"))})
    state = ROOT / "codex/state"
    with (state / "world-diff-findings.tsv").open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["entry", "checkpoint_start", "key", "host", "guest", "wrong_side", "source_commit", "run", "evidence", "final_status"], delimiter="\t")
        writer.writeheader()
        writer.writerows(findings)
    coverage = {"manifests": manifests, "scenarios": scenarios, "findings": len(findings)}
    (state / "world-diff-coverage.json").write_text(json.dumps(coverage, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("scenarios", len(scenarios), "findings", len(findings), "manifests", len(manifests))
    return coverage


if __name__ == "__main__":
    try:
        collect()
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print("INVALID findings:", exc, file=sys.stderr)
        sys.exit(2)
