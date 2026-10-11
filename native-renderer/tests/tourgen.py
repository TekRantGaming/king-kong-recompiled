#!/usr/bin/env python3
"""Writes tests/tour.json: the chapter tour, every chapter-select entry (0-41) as a golden scene.

The tour plays several chapters per launch: from the chapter select it starts a chapter, skips its movies,
waits for the chosen moment, takes the shots, then quits through the pause menu (Quit game, Yes) back to the
chapter select for the next chapter. KK_DEV_SCRIPT is a list of timed presses, so every
step has a fixed time with enough slack for the slowest load seen; all times count from the save menu opening.
The confirmation goes straight back to the chapter select, its cursor on the chapter just played.

  python -I tourgen.py                 write tour.json beside this file
  python -I tourgen.py --probe <out>   a probe file instead: shots every few seconds through each chapter
                                       (--chapters 1,2,3 --every 3 --stay 120), to find moments offline
  python -I tourgen.py --check         print the batches with their start times and length

Chapter table, moments and batches: CHAPTERS, MOMENTS and BATCHES below; docs/testing.md ("The chapter tour")
explains how they were found.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Chapter-select order: index = RIGHT presses from the first entry.
CHAPTERS = [
    "Venture", "Skull Island", "Necropolis", "Scorpions", "The Wall", "Sacrifice", "On Kong's Tracks", "Hayes",
    "V-Rex", "Ann", "Kong", "The Canyon", "Millipedes", "Brontosaurus", "Jimmy", "On the Raft", "Rapids", "Fight",
    "Swamps", "Chased by V-Rex", "The Log", "The Skull Islanders", "To Save Ann", "The Cave", "Venatosaurus",
    "In the Mud", "Call Kong", "Kong to the Rescue", "To the Plane", "To the Lair", "Kong's Lair",
    "Fight in the Lair", "Free!", "Chased by Kong", "Heading Back", "Back to the Village", "Kong's Capture",
    "Kong Struck Down", "In the Streets of New York", "The Empire State Building", "Kong's Death", "Credits",
]
SLUGS = [
    "venture", "skull_island", "necropolis", "scorpions", "the_wall", "sacrifice", "kongs_tracks", "hayes",
    "vrex", "ann", "kong", "canyon", "millipedes", "brontosaurus", "jimmy", "raft", "rapids", "fight", "swamps",
    "chased_vrex", "the_log", "islanders", "save_ann", "the_cave", "venatosaurus", "in_the_mud", "call_kong",
    "kong_rescue", "to_the_plane", "to_the_lair", "kongs_lair", "lair_fight", "free", "chased_by_kong",
    "heading_back", "back_village", "kong_capture", "kong_struck", "new_york", "empire_state", "kong_death",
    "credits",
]
assert len(CHAPTERS) == 42 and len(SLUGS) == 42


def scene_name(idx: int) -> str:
    return f"tour{idx:02d}_{SLUGS[idx]}"


# ---------------------------------------------------------------------------
# Menu timing (seconds). Measured in the probes (docs/testing.md, "The chapter tour").

FIRST_SELECT = 30.0  # the chapter select is open (12:A save, 18:A loaded, 24:A Play)
MOVE_STEP = 0.6  # between RIGHT / LEFT presses on the chapter select
MOVE_SETTLE = 2.0  # after the last move, before A
QUIT_SEQUENCE = [(0.0, "START"), (1.5, "UP"), (2.2, "UP"), (3.0, "A"), (5.0, "A")]  # pause, Quit game, Yes
# Yes goes straight back to the chapter select (no main menu), with the cursor on the chapter just played;
# it shows about 3 s after Yes.
QUIT_TO_SELECT = 10.0  # from START until the chapter select takes moves
SKIP_EVERY = 2.0  # SKIP (Start while a movie plays) presses
SKIP_FROM, SKIP_TO = 4.0, 40.0  # after the chapter's A


# ---------------------------------------------------------------------------
# Per chapter: the moment (seconds after the chapter's A) and what it shows. Filled in from the probes.
#   shots:  the test shot times (3-5)
#   burst:  golden-only frames around them (from, to, step); default shots[0]-1 .. shots[-1]+1, 0.2
#   stay:   when to quit (default last shot + 3)
#   skip:   (from, to) of the SKIP presses, or None for none
#   presses: extra presses, "t:BUTTONS[/hold]" relative to the chapter's A
#   kind:   gameplay, cutscene or menu; what: the picture
#   thresholds: overrides of TOUR_THRESHOLDS
MOMENTS: dict[int, dict] = {}

# Thresholds of an animated gameplay or cutscene moment, as for the chap04-chap33 scenes; tuned per scene from
# the Xenos self-check (calibrate).
TOUR_THRESHOLDS = {
    "mae": None, "ssim": None, "tile_mae": None, "bad": None,
    "min_pass": 0.6, "env_tol": 32, "env_flash_level": 6,
}

# Launches: chapters played in one launch, in order. Every batch starts from the chapter select on entry 0.
BATCHES: list[list[int]] = []
# The scene whose frame log a batch takes (one window per launch); the first of the batch if not listed.
FRAME_LOG: dict[int, float] = {}


# ---------------------------------------------------------------------------


def fmt(t: float) -> str:
    return f"{round(t, 1):.1f}".rstrip("0").rstrip(".")


def build_batch(chapters, moment_of, every=None, probe_stay=None):
    """Script presses and per-chapter absolute shot times for one launch.

    moment_of(idx) gives the chapter's moment dict. Returns (script entries, [(idx, start, times, golden)])."""
    presses: list[tuple[float, str]] = [(12.0, "A"), (18.0, "A"), (24.0, "A")]
    t = FIRST_SELECT
    cursor = 0
    out = []
    for n, idx in enumerate(chapters):
        m = moment_of(idx)
        moves = idx - cursor
        button = "RIGHT" if moves > 0 else "LEFT"
        for i in range(abs(moves)):
            presses.append((t + i * MOVE_STEP, button))
        start = t + abs(moves) * MOVE_STEP + (MOVE_SETTLE if moves else 0.5)
        presses.append((start, "A"))
        skip = m.get("skip", (SKIP_FROM, SKIP_TO))
        if skip:
            s = skip[0]
            while s <= skip[1] + 1e-6:
                presses.append((start + s, "SKIP"))
                s += SKIP_EVERY
        for p in m.get("presses", []):
            rel, rest = p.split(":", 1)
            presses.append((start + float(rel), rest))
        if probe_stay is not None:
            times = []
            s = 3.0
            while s <= probe_stay + 1e-6:
                times.append(round(start + s, 1))
                s += every
            golden = []
            stay = probe_stay + 1.0
        else:
            times = [round(start + s, 1) for s in m["shots"]]
            b = m.get("burst")
            if b is None:
                b = (m["shots"][0] - 1.0, m["shots"][-1] + 1.0, 0.2)
            golden = []
            g = b[0]
            while g <= b[1] + 1e-6:
                golden.append(round(start + g, 1))
                g += b[2]
            stay = m.get("stay", m["shots"][-1] + 3.0)
        out.append((idx, start, times, golden))
        if n == len(chapters) - 1:
            break
        q = start + stay
        for rel, b in QUIT_SEQUENCE:
            presses.append((q + rel, b))
        t = q + QUIT_TO_SELECT
        cursor = idx
    presses.sort(key=lambda p: p[0])
    script = ",".join(f"{fmt(at)}:{b}" for at, b in presses)
    return script, out


def base_file() -> dict:
    with open(HERE / "scenes.json", encoding="utf-8") as f:
        base = json.load(f)
    return {
        "version": 1,
        "about": "The chapter tour (written by tests/tourgen.py, do not edit by hand): every chapter-select entry "
                 "as a scene, several chapters per launch. Times are seconds after the save menu opens. See "
                 "docs/testing.md, 'The chapter tour'.",
        "defaults": base["defaults"],
        "runs": {},
        "scenes": [],
    }


def make_tour() -> dict:
    data = base_file()
    for b, chapters in enumerate(BATCHES):
        run = f"tour{chr(ord('A') + b)}"
        script, out = build_batch(chapters, lambda i: MOMENTS[i])
        log_idx = next((i for i in chapters if i in FRAME_LOG), chapters[0])
        run_def = {"from": "menu", "script": script}
        for idx, start, times, golden in out:
            m = MOMENTS[idx]
            if idx == log_idx:
                at = FRAME_LOG.get(idx, times[len(times) // 2] - start)
                run_def["frame_log"] = {"scene": scene_name(idx), "at": round(start + at, 1), "frames": 2}
            th = dict(TOUR_THRESHOLDS)
            th.update(m.get("thresholds", {}))
            scene = {
                "name": scene_name(idx),
                "run": run,
                "chapter": idx,
                "kind": m["kind"],
                "description": f"Chapter-select entry {idx}, {CHAPTERS[idx]}: {m['what']}",
                "chapter_start": round(start, 1),
                "times": times,
                "golden_times": sorted(set(golden) - set(times)),
                "thresholds": th,
            }
            if m.get("ignore"):
                scene["ignore"] = m["ignore"]
            data["scenes"].append(scene)
        data["runs"][run] = run_def
    return data


def make_probe(chapters, every, stay, noskip=()) -> dict:
    data = base_file()
    data["about"] = "A probe of the chapter tour (tests/tourgen.py --probe): shots every few seconds."
    data["defaults"]["startup_allowance"] = 120
    def moment(i):
        m = dict(MOMENTS.get(i, {}))
        if i in noskip:
            m["skip"] = None
        return m

    script, out = build_batch(chapters, moment, every=every, probe_stay=stay)
    data["runs"]["probe"] = {"from": "menu", "script": script}
    for idx, start, times, _golden in out:
        data["scenes"].append({"name": f"probe{idx:02d}", "run": "probe", "chapter": idx,
                               "chapter_start": round(start, 1), "description": CHAPTERS[idx],
                               "times": times, "thresholds": {}})
    return data


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--probe", default="", help="write a probe file here instead of tour.json")
    p.add_argument("--chapters", default="", help="probe: chapter indices, in play order")
    p.add_argument("--every", type=float, default=3.0, help="probe: seconds between shots")
    p.add_argument("--stay", type=float, default=120.0, help="probe: seconds per chapter")
    p.add_argument("--noskip", default="", help="probe: chapters whose movies are not skipped")
    p.add_argument("--check", action="store_true", help="print the batches and their timing")
    p.add_argument("--out", default=str(HERE / "tour.json"))
    a = p.parse_args(argv)
    if a.probe:
        chapters = [int(c) for c in a.chapters.split(",") if c.strip()]
        noskip = {int(c) for c in a.noskip.split(",") if c.strip()}
        data = make_probe(chapters, a.every, a.stay, noskip)
        path = Path(a.probe)
    else:
        missing = [i for i in range(42) if i not in MOMENTS]
        data = make_tour()
        path = Path(a.out)
        if missing:
            print(f"no moment yet for chapters {missing}", file=sys.stderr)
    if a.check:
        for run, r in data["runs"].items():
            scenes = [s for s in data["scenes"] if s["run"] == run]
            last = max(t for s in scenes for t in s["times"])
            print(f"{run}: {len(scenes)} chapters, last shot at {last:.0f} s, script {len(r['script'])} chars")
            for s in scenes:
                print(f"  {s['name']:28} start {s['chapter_start']:7.1f}  shots {s['times'][0]:.1f}..{s['times'][-1]:.1f}")
        return 0
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=1)
        f.write("\n")
    print(f"wrote {path}: {len(data['runs'])} runs, {len(data['scenes'])} scenes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
