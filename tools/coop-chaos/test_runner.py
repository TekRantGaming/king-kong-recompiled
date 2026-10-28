"""Offline checks of assertion-only mutation verdicts and source registration."""
from pathlib import Path
import runpy
import unittest

ROOT = Path(__file__).resolve().parents[2]
RUNNER = runpy.run_path(str(ROOT / "tools" / "run-twins.py"))


class RunnerTests(unittest.TestCase):
    def test_only_assertion_exit_is_caught(self):
        caught = RUNNER["is_caught"]
        rx = r"FAIL \[guard\]:"
        self.assertTrue(caught(1, "FAIL [guard]: stale world", False, rx))
        for code in (0, 2, 127, -1, 0xC0000005, 0xC0000135, 0xC000007B):
            self.assertFalse(caught(code, "FAIL [guard]: stale world", False, rx))
        self.assertFalse(caught(1, "timeout", True, rx))
        self.assertFalse(caught(1, "FAIL [guard]: stale world", True, rx))
        self.assertFalse(caught(1, "import error", False, rx))
        self.assertFalse(caught(1, "FAIL [wrong_guard]: other check", False, rx))
        self.assertFalse(caught(1, "FAIL [guard]: stale world", False, None))

    def test_all_guard_sites_are_registered(self):
        table = RUNNER["load_table"](ROOT / "tools" / "coop-chaos" / "twins.json")
        self.assertEqual(27, len(table["twins"]))
        self.assertEqual([], RUNNER["scan_problems"](table))
        table["twins"] = table["twins"][:-1]
        self.assertTrue(any("UNLISTED" in item for item in RUNNER["scan_problems"](table)))


if __name__ == "__main__":
    unittest.main()
