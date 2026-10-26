"""Private native-world oracle, Python312 -I. stdout contains named differences only.

Exit 0: supported shared fields equal (partial coverage); 1: settled desync;
2: invalid/incomplete evidence; 3: policy permits independent worlds.
Use --series with role directories, --json to retain validity and coverage.
No raw RAM comparison and no replicated health/cache substitution.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import sys

SCHEMA = 1
IGNORED_ENEMY = {"position", "native_state", "scheduled"}
IDENTITY = ("session", "generation", "level", "start", "enemy_visit", "checkpoint",
            "shared_checkpoint", "completed", "policy")


def read(path):
    def pairs(values):
        out = {}
        for key, value in values:
            if key in out:
                raise ValueError(f"duplicate JSON key {key}")
            out[key] = value
        return out
    return json.loads(Path(path).read_text(encoding="utf-8-sig"), object_pairs_hook=pairs,
                      parse_constant=lambda x: (_ for _ in ()).throw(ValueError(f"nonfinite {x}")))


def finite_tree(value):
    if isinstance(value, float):
        return math.isfinite(value)
    if isinstance(value, dict):
        return all(finite_tree(v) for v in value.values())
    if isinstance(value, list):
        return all(finite_tree(v) for v in value)
    return True


def validate(d, role=None):
    errors = []
    if not isinstance(d, dict):
        return ["snapshot is not an object"]
    if d.get("schema") != SCHEMA or d.get("protocol") != 21:
        errors.append("unsupported schema/protocol")
    if role and d.get("role") != role:
        errors.append("wrong peer role")
    if d.get("valid") is not True or d.get("errors") != []:
        errors.append(f"native capture invalid: {d.get('errors')}")
    if not finite_tree(d):
        errors.append("nonfinite value")
    try:
        ident = d["identity"]
        for key in IDENTITY:
            if key not in ident:
                errors.append("missing identity." + key)
        if not int(ident["session"], 16) or min(ident["generation"], ident["level"], ident["enemy_visit"]) <= 0:
            errors.append("zero native/session identity")
        if not isinstance(d["sample"], int) or d["sample"] <= 0 or d["captured_utc"] <= 0 or d["playing_seconds"] < 1:
            errors.append("invalid sample/time identity")
        evidence = d["evidence"]
        for key, size in (("source_commit", 40), ("build_commit", 40), ("exe_sha256", 64)):
            value = evidence[key]
            if len(value) != size or any(c not in "0123456789abcdef" for c in value):
                errors.append("missing/invalid evidence." + key)
        if evidence["build_commit"] != evidence["source_commit"]:
            errors.append("stale executable build marker")
        if "source_dirty" not in evidence or not evidence["scenario"] or not evidence["fault_profile"]:
            errors.append("missing dirty/scenario/fault evidence")
        enemies, story = d["shared"]["enemies"], d["shared"]["story"]
        if enemies["unbound"]:
            errors.append("incomplete native enemy census")
        if not isinstance(story["watermark"], int) or story["watermark"] < 0 or story["ready"] is not True:
            errors.append("story not load-ready/settled")
        if not isinstance(enemies["rows"], dict) or not isinstance(story["rows"], dict):
            errors.append("missing native row maps")
        elif not enemies["rows"] and not story["rows"]:
            errors.append("empty shared native world")
        for key, row in enemies["rows"].items():
            source, sequence, family = key.split(":")
            if not int(source, 16) or int(sequence) < 0 or not 1 <= int(family) <= 14 or row["family"] != int(family):
                errors.append("invalid enemy id " + key)
            initializing = row.get("phase") == "initializing"
            for field in ("health", "secondary_health"):
                if initializing and row[field] is None:
                    continue
                if not isinstance(row[field], (int, float)) or isinstance(row[field], bool):
                    errors.append("unreadable native " + field)
            if (type(row["alive"]) is not bool and not (initializing and row["alive"] is None)) or row["owner"] not in ("host", "guest", "unassigned") or row["owner_generation"] < 0:
                errors.append("invalid enemy lifecycle/owner")
        if sum(row.get("phase") == "initializing" for row in enemies["rows"].values()) != enemies["initializing"]:
            errors.append("incomplete initializer census")
        for key, row in story["rows"].items():
            source, script = key.split(":")
            if not int(source, 16) or not int(script, 16):
                errors.append("invalid script id " + key)
            for field in ("cursor", "run", "started", "finished"):
                if type(row[field]) is not int or row[field] < 0:
                    errors.append("invalid native script " + field)
            for field in ("loop", "detect", "activity"):
                if type(row[field]) is not bool:
                    errors.append("unreadable native script " + field)
        objects = d["shared"]["objects"]
        if not isinstance(objects["rows"], dict) or not isinstance(objects["bodies"], dict):
            errors.append("missing native object maps")
        for key, row in objects["rows"].items():
            source, kind = key.split(":")
            if not int(source, 16) or int(kind) != row["kind"] or type(row["available"]) is not bool:
                errors.append("invalid native object identity")
            if row["available"] and "flags" not in row:
                errors.append("missing native object flags")
        for key, row in objects["bodies"].items():
            if not int(key, 16) or type(row["available"]) is not bool or type(row["retired"]) is not bool:
                errors.append("invalid native body identity")
            if row["available"] and (len(row["pose"]) != 9 or any(type(v) not in (int, float) for v in row["pose"])):
                errors.append("unreadable native body pose")
        player = d["local"]["player"]
        if player["id"] != d["role"] or len(player["position"]) != 3 or not all(type(v) in (int, float) for v in player["position"]):
            errors.append("missing native local player")
        if not isinstance(d["native_checkpoint_ids"], list) or any(type(v) is not int or not 0 <= v <= 65535 for v in d["native_checkpoint_ids"]):
            errors.append("invalid native checkpoint registry")
        if not isinstance(d["coverage"], dict) or not d["coverage"]:
            errors.append("missing coverage contract")
    except (KeyError, TypeError, ValueError, AttributeError) as exc:
        errors.append("malformed snapshot: " + str(exc))
    return errors


def shared(d):
    """Native equality projection. Keep authority metadata, omit local AI scheduling.

    Position/state/scheduling remain in the dump for diagnosis. Different owner
    state machines and continuous movement are not a whole-world equality gate.
    Personal inventory/health/camera and body rig choices are intentionally local.
    """
    out = copy.deepcopy(d["shared"])
    # Reliable delivery/checksum traffic advances this counter even when no
    # native state changes. Pair boundaries must agree, but quiet native
    # windows need not hold an unchanged transport watermark.
    out["story"].pop("watermark", None)
    # Unbound diagnostics have no immutable identity, and are retained separately.
    # Their count still invalidates the whole-world oracle.
    out["enemies"].pop("unresolved", None)
    for row in out["enemies"]["rows"].values():
        for key in IGNORED_ENEMY:
            row.pop(key, None)
    for row in out.get("objects", {}).get("rows", {}).values():
        row.pop("seconds", None)
    return out


def differences(a, b, health_tol=0.01, path="shared"):
    out = []
    if isinstance(a, dict) and isinstance(b, dict):
        for key in sorted(a.keys() | b.keys()):
            p = path + "." + key
            if key not in a or key not in b:
                out.append({"key": p, "host": a.get(key, "<missing>"), "guest": b.get(key, "<missing>")})
            else:
                out.extend(differences(a[key], b[key], health_tol, p))
    elif isinstance(a, list) and isinstance(b, list) and len(a) == len(b):
        for i, (av, bv) in enumerate(zip(a, b)):
            out.extend(differences(av, bv, health_tol, path + "." + str(i)))
    else:
        tol = health_tol if path.rsplit(".", 1)[-1] in ("health", "secondary_health") else 0
        if ".objects." in path and (".pose." in path or path.endswith(".position")):
            tol = .02
        equal = type(a) is type(b) and a == b
        if type(a) in (int, float) and type(b) in (int, float):
            equal = abs(a-b) <= tol
        if not equal:
            out.append({"key": path, "host": a, "guest": b})
    return out


def compare(host, guest, health_tol=0.01, max_skew=3.0):
    errors = ["host: " + s for s in validate(host, "host")]
    errors += ["guest: " + s for s in validate(guest, "guest")]
    result = {"status": "INVALID", "errors": errors, "differences": [], "coverage": host.get("coverage", {}),
              "proof": "settled native supported-field equality only; not campaign progression or universal world parity"}
    if errors:
        return result
    if host["identity"]["policy"] != "together" or guest["identity"]["policy"] != "together":
        result["status"] = "UNSUPPORTED_POLICY"
        return result
    for key in IDENTITY:
        if host["identity"][key] != guest["identity"][key]:
            errors.append("unagreed identity." + key)
    for key in ("source_commit", "exe_sha256", "scenario", "fault_profile", "source_dirty"):
        if host["evidence"][key] != guest["evidence"][key]:
            errors.append("peer evidence differs: " + key)
    if host["coverage"] != guest["coverage"]:
        errors.append("peer coverage contracts differ")
    if abs(host["captured_utc"] - guest["captured_utc"]) > max_skew:
        errors.append("samples too far apart")
    if host["shared"]["story"]["watermark"] != guest["shared"]["story"]["watermark"]:
        errors.append("story watermark not agreed")
    if errors:
        return result
    result["differences"] = differences(shared(host), shared(guest), health_tol)
    result["status"] = "DIFF" if result["differences"] else "PARTIAL_EQUAL"
    return result


def stable_candidates(rows, count, health_tol):
    out = []
    for i in range(count-1, len(rows)):
        window = rows[i-count+1:i+1]
        if any(validate(d) for _, d in window):
            continue
        first = window[0][1]
        if window[-1][1]["captured_utc"] - first["captured_utc"] < 1:
            continue
        if len({d["sample"] for _, d in window}) != count:
            continue
        if any(d["captured_utc"] <= p["captured_utc"] or d["playing_seconds"] <= p["playing_seconds"]
               for (_, p), (_, d) in zip(window, window[1:])):
            continue
        if any(d["identity"] != first["identity"] or differences(shared(first), shared(d), health_tol) for _, d in window[1:]):
            continue
        out.append((rows[i], [p for p, _ in window]))
    return out


def series(host_dir, guest_dir, count=3, health_tol=0.01, max_skew=3.0):
    def rows(directory):
        return sorted(((str(p), read(p)) for p in Path(directory).glob("world-g*.json")), key=lambda p: p[1].get("captured_utc", 0))
    h, g = rows(host_dir), rows(guest_dir)
    invalid = {"status": "INVALID", "errors": [], "differences": [], "host_samples": len(h), "guest_samples": len(g)}
    if not h or not g:
        invalid["errors"] = ["missing peer snapshots"]
        return invalid
    hs, gs = stable_candidates(h, count, health_tol), stable_candidates(g, count, health_tol)
    rejected, accepted = [], []
    candidates = sorted(((abs(a[0][1]["captured_utc"]-b[0][1]["captured_utc"]), a, b) for a in hs for b in gs), key=lambda p: p[0])
    for _, ((hp, hd), hw), ((gp, gd), gw) in candidates:
        result = compare(hd, gd, health_tol, max_skew)
        if result["status"] != "INVALID":
            result.update(host=hp, guest=gp, host_window=hw, guest_window=gw, quiet_samples=count,
                          identity=hd["identity"], watermark=hd["shared"]["story"]["watermark"],
                          host_samples=len(h), guest_samples=len(g))
            accepted.append(result)
            continue
        rejected.extend(result["errors"])
    if accepted:
        # Keep every named mismatch observed in any settled pair, even if C72
        # later heals it. Choose one nearest-time guest window per host sample.
        nearest = {}
        for result in accepted:
            nearest.setdefault(result["host"], result)
        observations = sorted(nearest.values(), key=lambda r: read(r["host"])["captured_utc"])
        found = {}
        for result in observations:
            for diff in result["differences"]:
                key = json.dumps(diff, sort_keys=True)
                found.setdefault(key, diff)
        selected = copy.deepcopy(next((r for r in observations if r["differences"]), observations[-1]))
        selected["final_status"] = observations[-1]["status"]
        selected["status"] = "DIFF" if found else selected["status"]
        selected["differences"] = list(found.values())
        selected["settled_comparisons"] = len(observations)
        selected["observations"] = [{k: r[k] for k in ("host", "guest", "watermark", "status", "differences")} for r in observations]
        # An early match is not evidence for a later unreadable, unagreed or
        # continuously evolving world. Keep known mismatches, but require a
        # fresh quiet comparison at the end before reporting equality.
        final = compare(h[-1][1], g[-1][1], health_tol, max_skew)
        tail_errors = list(final["errors"])
        last = observations[-1]
        if any(read(last[role])["captured_utc"] < rows[-1][1]["captured_utc"] - max_skew
               for role, rows in (("host", h), ("guest", g))):
            tail_errors.append("final quiet native comparison is stale")
        if tail_errors:
            selected["errors"] = sorted(set(tail_errors))
            selected["final_status"] = "INVALID"
            selected["status"] = "DIFF" if found else "INVALID"
            selected["final_captures"] = {"host": h[-1][0], "guest": g[-1][0]}
        return selected
    invalid["errors"] = sorted(set(rejected or ["no three fresh quiet native samples on each peer"]))
    invalid["native_errors"] = {"host": sorted(set(e for _, d in h for e in validate(d, "host"))),
                                "guest": sorted(set(e for _, d in g for e in validate(d, "guest")))}
    return invalid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("guest")
    parser.add_argument("--series", action="store_true")
    parser.add_argument("--quiet-samples", type=int, default=3)
    parser.add_argument("--health-tolerance", type=float, default=0.01)
    parser.add_argument("--max-skew", type=float, default=3)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    try:
        if not math.isfinite(args.health_tolerance) or args.health_tolerance < 0 or not math.isfinite(args.max_skew) or args.max_skew < 0 or args.quiet_samples < 2:
            raise ValueError("invalid tolerance/quiet settings")
        result = series(args.host, args.guest, args.quiet_samples, args.health_tolerance, args.max_skew) if args.series else compare(read(args.host), read(args.guest), args.health_tolerance, args.max_skew)
    except (OSError, ValueError, TypeError, KeyError) as exc:
        result = {"status": "INVALID", "errors": [str(exc)], "differences": []}
    if args.json:
        args.json.write_text(json.dumps(result, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")
    for diff in result["differences"]:
        print(f"{diff['key']}: host={json.dumps(diff['host'], sort_keys=True)} guest={json.dumps(diff['guest'], sort_keys=True)}")
    if result["status"] in ("INVALID", "UNSUPPORTED_POLICY"):
        print(result["status"] + ": " + "; ".join(result.get("errors", [])), file=sys.stderr)
    return {"PARTIAL_EQUAL": 0, "DIFF": 1, "INVALID": 2, "UNSUPPORTED_POLICY": 3}[result["status"]]


if __name__ == "__main__":
    sys.exit(main())
