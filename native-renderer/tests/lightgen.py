#!/usr/bin/env python3
"""Writes tests/lighting_tour.json: the chapters the lighting options are judged on (docs/lighting.md).

Each run is one launch that plays its chapters from the chapter select as the chapter tour does (tests/tourgen.py,
whose menu timing and moments this copies, like aogen.py): start a chapter, skip its movies, take three shots at a
calm moment, quit through the pause menu (Quit game, Yes) back to the chapter select, on to the next. Venture comes
last (its pause menu is different). Every setting is one launch of the same run, so the shots of two settings show
the same moment of the same chapter (the game's own frames, KK_DEV_SHOTS).

Runs:
  light    every chapter below (finding the passes with --native_lighting_census, about 14 minutes)
  lshadow  sunlit chapters for the shadows
  lwater   the water chapters for the reflections
  lshaft   the chapters with light shafts or mist for the volumetrics
  lsmoke   Skull Island, V-Rex and Ann: one launch for the comparison sheets of all three

  python -I lightgen.py            write lighting_tour.json beside this file
  python -I lightgen.py --check    print each run's chapters with their start times and the launch's length

Run it with tests/run.ps1 -ScenesFile native-renderer/tests/lighting_tour.json -Scenes <run> -NoCompare.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Chapter-select entry: (slug, seconds after the chapter's A of the first shot, what it shows, why it is here).
# Moments from the chapter tour (docs/testing.md, "The chapter tour").
CHAPTERS = {
    1: ("skull_island", 28.0, "Ann's face leaning over Jack on the beach", "sunlit, characters"),
    4: ("the_wall", 30.0, "a jungle clearing at dusk, a spiked wooden gate against the sky", "jungle, sky"),
    8: ("vrex", 40.0, "Jack in the rain under the cave roof, random lightning", "shadows, rain"),
    9: ("ann", 30.0, "a cave mouth with shafts of light falling in", "light shafts"),
    11: ("canyon", 30.0, "a canyon path between mossy rocks, a pool beyond", "pool, mist"),
    12: ("millipedes", 30.0, "a dark jungle path towards a lit gate in a wall of vines", "mist, jungle"),
    23: ("the_cave", 30.0, "a dark wooden scaffold over a pool, figures behind it", "water"),
    26: ("call_kong", 30.0, "a dark hall with a lattice of beams over pale water", "water, beams"),
    27: ("kong_rescue", 13.0, "Jack with a spear behind the rocks, a V-Rex, fires and rain", "water, fire, shafts"),
    29: ("to_the_lair", 30.0, "a great tree's roots in green mist", "mist, jungle"),
    33: ("chased_by_kong", 30.0, "a cave with a water channel, dark green rock", "water channel"),
    36: ("kong_capture", 30.0, "Kong from behind in a misty gorge, a wall of rock and roots beyond", "mist, Kong"),
    37: ("kong_struck", 30.0, "the Venture at anchor in the bay, seen from the rocks of the shore at dusk", "sea"),
    0: ("venture", 89.0, "the hull of the rocking ship with VENTURE painted on it, rain", "the sea"),
}
RUNS = {
    "light": [1, 4, 8, 9, 11, 12, 23, 26, 27, 29, 33, 36, 37, 0],
    "lshadow": [1, 4, 8, 9, 12, 29, 36],
    "lwater": [11, 23, 26, 27, 33, 37, 0],
    "lshaft": [4, 9, 12, 26, 27, 29, 36],
    "lsmoke": [1, 8, 9],
}
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


def build(run: str, order: list[int]):
    presses = [(12.0, "A"), (18.0, "A"), (24.0, "A")]
    t, cursor, scenes = FIRST_SELECT, 0, []
    end = 0.0
    for n, idx in enumerate(order):
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
            "name": f"{run}{idx:02d}_{slug}",
            "run": run,
            "chapter": idx,
            "kind": "gameplay",
            "description": f"Chapter-select entry {idx}: {what} ({why})",
            "chapter_start": round(start, 1),
            "times": times,
            "golden_times": [],
            "thresholds": {"mae": None, "ssim": None, "tile_mae": None, "bad": None, "min_pass": 0.6},
        })
        if n == len(order) - 1:
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
    p.add_argument("--out", default=str(HERE / "lighting_tour.json"))
    a = p.parse_args(argv)
    runs, all_scenes = {}, []
    for run, order in RUNS.items():
        script, scenes, end = build(run, order)
        if a.check:
            print(f"== {run}: about {end + 60:.0f} s with the start")
            for s in scenes:
                print(f"  {s['name']:28s} start {s['chapter_start']:6.1f}  shots {s['times']}")
            continue
        runs[run] = {"from": "menu", "script": script, "env": {"KK_DEV_FREEZE_SKIP": "10"}}
        all_scenes += scenes
    if a.check:
        return 0
    with open(HERE / "scenes.json", encoding="utf-8") as f:
        base = json.load(f)
    data = {
        "version": 1,
        "about": "The lighting options' chapters (written by tests/lightgen.py, do not edit by hand). Times are "
                 "seconds after the save menu opens. See docs/lighting.md.",
        "defaults": base["defaults"],
        "runs": runs,
        "scenes": all_scenes,
    }
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=1)
        f.write("\n")
    print(f"wrote {a.out}: {len(runs)} runs, {len(all_scenes)} scenes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
