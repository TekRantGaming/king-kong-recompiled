#!/usr/bin/env python3
"""Writes tests/ao_tour.json: the chapters the ambient occlusion is judged on (docs/ao.md, "In the game").

One launch plays these chapters from the chapter select, as the chapter tour does (tests/tourgen.py on the tour
branch, whose menu timing and moments this copies): start a chapter, skip its movies, take three shots at a calm
moment, quit through the pause menu (Quit game, Yes) back to the chapter select, on to the next. Venture comes
last (its pause menu is different). Every AO setting is one launch of the same script, so the shots of two
settings show the same moment of the same chapter (the game's own frames, KK_DEV_SHOTS).

  python -I aogen.py            write ao_tour.json beside this file
  python -I aogen.py --check    print the chapters with their start times and the launch's length

Run it with tests/run.ps1 -ScenesFile native-renderer/tests/ao_tour.json -Scenes ao -NoCompare.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Chapter-select entry: (slug, seconds after the chapter's A of the first shot, what it shows, why it is here).
CHAPTERS = {
    4: ("the_wall", 30.0, "a jungle clearing at dusk, a spiked wooden gate against the sky", "sky, foliage, distant terrain"),
    8: ("vrex", 40.0, "Jack in the rain under the cave roof, random lightning", "rain, rock, grass, the legacy AO's home scene"),
    10: ("kong", 12.0, "Kong crouching on a ledge by a ruined wall", "Kong's fur, ruins"),
    12: ("millipedes", 30.0, "a dark jungle path towards a lit gate in a wall of vines", "mist, foliage"),
    26: ("call_kong", 30.0, "a dark hall with a lattice of beams over pale water", "water under beams"),
    27: ("kong_rescue", 13.0, "Jack with a spear behind the rocks, a V-Rex, fires and rain", "fire, smoke, rain, characters"),
    33: ("chased_by_kong", 30.0, "a cave with a water channel, dark green rock", "water channel"),
    37: ("kong_struck", 30.0, "the Venture at anchor in the bay, seen from the rocks of the shore at dusk", "sea, distant terrain, sky"),
    0: ("venture", 89.0, "the hull of the rocking ship with VENTURE painted on it, rain", "sea spray, rain, the ship"),
}
ORDER = [4, 8, 10, 12, 26, 27, 33, 37, 0]
SHOTS = [0.0, 0.5, 1.0]

# Menu timing (seconds), as the chapter tour measured it.
FIRST_SELECT = 30.0
MOVE_STEP = 0.6
MOVE_SETTLE = 2.0
QUIT_SEQUENCE = [(0.0, "START")] + [(1.5 + 0.5 * i, "UP") for i in range(7)] + [(5.5, "A"), (7.5, "A")]
QUIT_TO_SELECT = 13.0
SKIP_EVERY = 2.0
SKIP_FROM, SKIP_TO = 4.0, 40.0


def fmt(t: float) -> str:
    return f"{round(t, 1):.1f}".rstrip("0").rstrip(".")


def build():
    presses = [(12.0, "A"), (18.0, "A"), (24.0, "A")]
    t, cursor, scenes = FIRST_SELECT, 0, []
    for n, idx in enumerate(ORDER):
        slug, moment, what, why = CHAPTERS[idx]
        moves = idx - cursor
        button = "RIGHT" if moves > 0 else "LEFT"
        for i in range(abs(moves)):
            presses.append((t + i * MOVE_STEP, button))
        start = t + abs(moves) * MOVE_STEP + (MOVE_SETTLE if moves else 0.5)
        presses.append((start, "A"))
        stay = moment + SHOTS[-1] + 3.0
        s = SKIP_FROM
        while s <= min(SKIP_TO, stay - 1.0) + 1e-6:
            presses.append((start + s, "SKIP"))
            s += SKIP_EVERY
        times = [round(start + moment + d, 1) for d in SHOTS]
        scenes.append({
            "name": f"ao{idx:02d}_{slug}",
            "run": "ao",
            "chapter": idx,
            "kind": "gameplay",
            "description": f"Chapter-select entry {idx}: {what} ({why})",
            "chapter_start": round(start, 1),
            "times": times,
            "golden_times": [],
            "thresholds": {"mae": None, "ssim": None, "tile_mae": None, "bad": None, "min_pass": 0.6},
        })
        if n == len(ORDER) - 1:
            end = start + stay
            break
        q = start + stay
        for rel, b in QUIT_SEQUENCE:
            presses.append((q + rel, b))
        t = q + QUIT_TO_SELECT
        cursor = idx
    presses.sort(key=lambda p: p[0])
    return ",".join(f"{fmt(a)}:{b}" for a, b in presses), scenes, end


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--check", action="store_true")
    p.add_argument("--out", default=str(HERE / "ao_tour.json"))
    a = p.parse_args(argv)
    script, scenes, end = build()
    if a.check:
        for s in scenes:
            print(f"{s['name']:24s} start {s['chapter_start']:6.1f}  shots {s['times']}")
        print(f"launch: about {end + 60:.0f} s with the start")
        return 0
    with open(HERE / "scenes.json", encoding="utf-8") as f:
        base = json.load(f)
    data = {
        "version": 1,
        "about": "The ambient occlusion's chapters (written by tests/aogen.py, do not edit by hand). Times are seconds "
                 "after the save menu opens. See docs/ao.md.",
        "defaults": base["defaults"],
        "runs": {"ao": {"from": "menu", "script": script, "env": {"KK_DEV_FREEZE_SKIP": "10"}}},
        "scenes": scenes,
    }
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=1)
        f.write("\n")
    print(f"wrote {a.out}: {len(scenes)} chapters, about {end + 60:.0f} s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
