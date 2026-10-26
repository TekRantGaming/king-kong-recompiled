"""Retained diagnostic fixtures, no deletion, no network, no third-party modules."""
import copy
import importlib.util
import json
from pathlib import Path
import unittest
import uuid
import sys
sys.dont_write_bytecode = True

spec = importlib.util.spec_from_file_location("world_diff", Path(__file__).with_name("world-diff.py"))
w = importlib.util.module_from_spec(spec)
spec.loader.exec_module(w)
E = "0000000000001234:0:1"
S = "0000000000005678:0000000000009abc"


def fixture(role="host"):
    return {"schema": 1, "protocol": 21, "role": role, "sample": 1, "captured_utc": 1000., "playing_seconds": 25.,
            "identity": {"session": "0000000000000001", "generation": 1, "level": 303, "start": 3,
                         "enemy_visit": 1, "checkpoint": 3, "shared_checkpoint": 3, "completed": "0000000000000000", "policy": "together"},
            "evidence": {"source_commit": "a"*40, "build_commit": "a"*40, "source_dirty": "", "exe_sha256": "b"*64,
                         "scenario": "e7-start3", "fault_profile": "150,30,3"},
            "coverage": {"objects": "unsupported:C71-not-integrated"}, "valid": True, "errors": [],
            "native_checkpoint_ids": [1, 3],
            "shared": {"enemies": {"unbound": 0, "initializing": 0, "rows": {E: {"family": 1, "health": 50., "secondary_health": 0.,
                       "alive": True, "owner": "host", "owner_generation": 1, "native_state": 1234, "scheduled": True, "position": [1., 2., 3.]}}},
                       "objects": {"rows": {}, "bodies": {}},
                       "story": {"watermark": 4, "ready": True, "rows": {S: {"cursor": 1, "run": 1, "started": 1, "finished": 0,
                                  "loop": True, "detect": False, "activity": True}}}},
            "local": {"player": {"id": role, "position": [1., 2., 3.], "weapon": 2, "ammo": 50, "health": 100},
                      "partner_body": {"available": True}}}


class Contract(unittest.TestCase):
    def setUp(self):
        self.h, self.g = fixture(), fixture("guest")

    def result(self):
        return w.compare(self.h, self.g)

    def test_native_object_damage_and_missing_capture(self):
        key = "0000000000001234:1"
        row = dict(kind=1, available=True, flags=0, value=0, position=0, seconds=0)
        for d in (self.h, self.g):
            d["shared"]["objects"]["rows"][key] = row.copy()
        self.g["shared"]["objects"]["rows"][key]["value"] = 100
        self.assertEqual(self.result()["differences"][0]["key"], "shared.objects.rows." + key + ".value")
        del self.g["shared"]["objects"]
        self.assertEqual(self.result()["status"], "INVALID")

    def test_native_body_pose_tolerance_and_countdown(self):
        key = "0000000000001234"
        body = dict(available=True, retired=False, topology=key, flags=0, pose=[0.] * 9)
        for d in (self.h, self.g):
            d["shared"]["objects"]["bodies"][key] = copy.deepcopy(body)
        self.g["shared"]["objects"]["bodies"][key]["pose"][0] = .01
        self.assertEqual(self.result()["status"], "PARTIAL_EQUAL")
        self.g["shared"]["objects"]["bodies"][key]["pose"][0] = 1.
        self.assertEqual(self.result()["differences"][0]["key"], "shared.objects.bodies." + key + ".pose.0")

    def test_unchanged_native_world(self):
        self.assertEqual(self.result()["status"], "PARTIAL_EQUAL")

    def test_one_native_life_change_named_exactly(self):
        self.g["shared"]["enemies"]["rows"][E]["health"] = 40
        self.assertEqual(self.result()["differences"], [{"key": f"shared.enemies.rows.{E}.health", "host": 50., "guest": 40}])

    def test_native_flag_change_named_exactly(self):
        self.g["shared"]["story"]["rows"][S]["activity"] = False
        self.assertEqual([d["key"] for d in self.result()["differences"]], [f"shared.story.rows.{S}.activity"])

    def test_inventory_and_local_hero_differences_allowed(self):
        self.g["local"]["player"].update(ammo=2, health=5, weapon=1, position=[90, 80, 70])
        self.assertEqual(self.result()["status"], "PARTIAL_EQUAL")

    def test_owner_scheduling_and_transient_ai_state_are_observations(self):
        self.g["shared"]["enemies"]["rows"][E].update(scheduled=False, native_state=5678, position=[90, 80, 70])
        self.assertEqual(self.result()["status"], "PARTIAL_EQUAL")

    def test_health_tolerance(self):
        self.g["shared"]["enemies"]["rows"][E]["health"] += .001
        self.assertEqual(self.result()["status"], "PARTIAL_EQUAL")

    def test_missing_enemy_is_named(self):
        self.g["shared"]["enemies"]["rows"] = {}
        self.assertEqual(self.result()["differences"][0]["key"], f"shared.enemies.rows.{E}")

    def test_alive_and_owner_cannot_hide(self):
        self.g["shared"]["enemies"]["rows"][E].update(alive=False, owner="guest", owner_generation=2)
        self.assertEqual(len(self.result()["differences"]), 3)

    def test_empty_world_invalid(self):
        self.g["shared"]["enemies"]["rows"] = {}
        self.g["shared"]["story"]["rows"] = {}
        self.assertEqual(self.result()["status"], "INVALID")

    def test_native_read_failure_invalid(self):
        self.g.update(valid=False, errors=["unbound-native-enemies:1"])
        self.assertEqual(self.result()["status"], "INVALID")

    def test_unbound_invalid_even_with_positive_rows(self):
        self.g["shared"]["enemies"]["unbound"] = 1
        self.assertEqual(self.result()["status"], "INVALID")

    def test_stale_executable_invalid(self):
        self.g["evidence"]["build_commit"] = "c"*40
        self.assertEqual(self.result()["status"], "INVALID")

    def test_missing_executable_sha_invalid(self):
        self.g["evidence"].pop("exe_sha256")
        self.assertEqual(self.result()["status"], "INVALID")

    def test_different_native_visit_invalid(self):
        self.g["identity"]["enemy_visit"] = 2
        self.assertEqual(self.result()["status"], "INVALID")

    def test_same_sequence_new_load_is_not_aligned(self):
        self.g["identity"]["generation"] = 2
        self.assertEqual(self.result()["status"], "INVALID")

    def test_watermark_not_agreed_invalid(self):
        self.g["shared"]["story"]["watermark"] = 5
        self.assertEqual(self.result()["status"], "INVALID")

    def test_timestamp_skew_invalid(self):
        self.g["captured_utc"] += 5
        self.assertEqual(self.result()["status"], "INVALID")

    def test_independent_policy_not_pass(self):
        self.h["identity"]["policy"] = self.g["identity"]["policy"] = "solo-regroup"
        self.assertEqual(self.result()["status"], "UNSUPPORTED_POLICY")

    def test_null_health_invalid(self):
        self.g["shared"]["enemies"]["rows"][E]["health"] = None
        self.assertEqual(self.result()["status"], "INVALID")

    def test_authored_initializer_has_explicit_not_applicable_health(self):
        for d in (self.h, self.g):
            d["shared"]["enemies"]["initializing"] = 1
            d["shared"]["enemies"]["rows"][E].update(phase="initializing", health=None, secondary_health=None, alive=None, owner="unassigned")
        self.assertEqual(self.result()["status"], "PARTIAL_EQUAL")

    def test_missing_initializer_rows_invalid(self):
        self.g["shared"]["enemies"]["initializing"] = 1
        self.assertEqual(self.result()["status"], "INVALID")

    def test_absent_authority_is_observed_difference(self):
        self.g["shared"]["enemies"]["rows"][E].update(owner="unassigned")
        self.assertEqual(self.result()["status"], "DIFF")
        self.assertEqual(self.result()["differences"][0]["key"], f"shared.enemies.rows.{E}.owner")

    def test_nonfinite_invalid(self):
        self.g["shared"]["enemies"]["rows"][E]["health"] = float("nan")
        self.assertEqual(self.result()["status"], "INVALID")

    def test_quiet_samples_require_fresh_time_and_sequence(self):
        rows = [(str(i), copy.deepcopy(self.h)) for i in range(3)]
        self.assertEqual(w.stable_candidates(rows, 3, .01), [])
        for i, (_, d) in enumerate(rows):
            d.update(sample=i+1, captured_utc=1000.+2*i, playing_seconds=25.+2*i)
        self.assertEqual(len(w.stable_candidates(rows, 3, .01)), 1)

    def test_quiet_rejects_evolving_world(self):
        rows = []
        for i in range(3):
            d = copy.deepcopy(self.h)
            d.update(sample=i+1, captured_utc=1000.+2*i, playing_seconds=25.+2*i)
            d["shared"]["enemies"]["rows"][E]["health"] -= i
            rows.append((str(i), d))
        self.assertEqual(w.stable_candidates(rows, 3, .01), [])

    def test_checksum_traffic_does_not_prevent_native_quiet(self):
        rows = []
        for i in range(3):
            d = copy.deepcopy(self.h)
            d.update(sample=i+1, captured_utc=1000.+i*2, playing_seconds=25.+i*2)
            d["shared"]["story"]["watermark"] += i*10
            rows.append((str(i), d))
        self.assertEqual(len(w.stable_candidates(rows, 3, .01)), 1)

    def retained_series(self, mutate=None):
        root = Path("F:/kk-native-coop/scratch") / ("c76-fixture-" + uuid.uuid4().hex)
        for role in ("host", "guest"):
            directory = root / role
            directory.mkdir(parents=True)
            for i in range(3):
                d = fixture(role)
                d.update(sample=i+1, captured_utc=1000.+i*2, playing_seconds=25.+i*2)
                if role == "guest" and mutate:
                    mutate(d)
                (directory / f"world-g1-{i+1}.json").write_text(json.dumps(d), encoding="utf-8")
        return root

    def test_real_file_series_unchanged(self):
        root = self.retained_series()
        result = w.series(root / "host", root / "guest")
        self.assertEqual(result["status"], "PARTIAL_EQUAL")
        self.assertEqual(len(result["host_window"]), 3)

    def test_real_file_series_planted_life(self):
        root = self.retained_series(lambda d: d["shared"]["enemies"]["rows"][E].update(health=40.))
        result = w.series(root / "host", root / "guest")
        self.assertEqual(result["status"], "DIFF")
        self.assertEqual([d["key"] for d in result["differences"]], [f"shared.enemies.rows.{E}.health"])

    def test_real_file_missing_peer_invalid(self):
        root = self.retained_series()
        self.assertEqual(w.series(root / "host", root / "missing")["status"], "INVALID")

    def test_duplicate_json_key_invalid(self):
        root = self.retained_series()
        path = root / "duplicate.json"
        path.write_text('{"valid":true,"valid":false}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate JSON key valid"):
            w.read(path)

    def test_earlier_desync_retained_after_healing(self):
        root = self.retained_series(lambda d: d["shared"]["enemies"]["rows"][E].update(health=40.))
        for role in ("host", "guest"):
            for i in range(3, 6):
                d = fixture(role)
                d.update(sample=i+1, captured_utc=1000.+i*2, playing_seconds=25.+i*2)
                (root / role / f"world-g1-{i+1}.json").write_text(json.dumps(d), encoding="utf-8")
        result = w.series(root / "host", root / "guest")
        self.assertEqual(result["status"], "DIFF")
        self.assertEqual(result["final_status"], "PARTIAL_EQUAL")
        self.assertEqual(len(result["differences"]), 1)
        self.assertEqual(result["observations"][-1]["status"], "PARTIAL_EQUAL")


if __name__ == "__main__":
    unittest.main()
