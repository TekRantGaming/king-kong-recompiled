"""Offline controls using an actual retained native snapshot. No game mutation."""
import copy
import importlib.util
import json
from pathlib import Path
import sys
sys.dont_write_bytecode = True

spec = importlib.util.spec_from_file_location("world_diff", Path(__file__).with_name("world-diff.py"))
w = importlib.util.module_from_spec(spec)
spec.loader.exec_module(w)


def prove(path):
    native = w.read(path)
    h, g = copy.deepcopy(native), copy.deepcopy(native)
    h["role"], g["role"] = "host", "guest"
    h["local"]["player"]["id"], g["local"]["player"]["id"] = "host", "guest"
    if w.validate(h, "host") or w.validate(g, "guest"):
        raise ValueError("native input capture is incomplete")
    results = {}
    def check(name, condition, details):
        results[name] = {"pass": bool(condition), "details": details}
        if not condition:
            raise ValueError(name + " failed: " + str(details))
    equal = w.compare(h, g)
    check("same_native_capture", equal["status"] == "PARTIAL_EQUAL" and not equal["differences"], equal["status"])
    g["local"]["player"].update(weapon=1, personal_ammo=1, personal_health=1)
    inventory = w.compare(h, g)
    check("personal_inventory_allowed", inventory["status"] == "PARTIAL_EQUAL", inventory["status"])
    candidates = [(key, row) for key, row in g["shared"]["enemies"]["rows"].items() if type(row["health"]) in (int, float)]
    if candidates:
        key, row = candidates[0]
        row["health"] += 1
        expected = "shared.enemies.rows." + key + ".health"
    else:
        key, row = next(iter(g["shared"]["story"]["rows"].items()))
        row["activity"] = not row["activity"]
        expected = "shared.story.rows." + key + ".activity"
    planted = w.compare(h, g)
    check("one_native_field_named", planted["status"] == "DIFF" and [d["key"] for d in planted["differences"]] == [expected], planted["differences"])
    g["evidence"]["build_commit"] = "0"*40
    stale = w.compare(h, g)
    check("stale_executable_refused", stale["status"] == "INVALID", stale["errors"])
    return {"native_source": str(path), "proof_kind": "offline copied-native capture controls, not two-engine mutation", "results": results}


if __name__ == "__main__":
    try:
        result = prove(Path(sys.argv[1]))
        Path(sys.argv[2]).write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print("PASS native offline controls", result["native_source"])
    except (OSError, ValueError, KeyError, TypeError, StopIteration) as exc:
        print("INVALID native proof:", exc, file=sys.stderr)
        sys.exit(2)
