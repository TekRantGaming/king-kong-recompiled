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
# The pause menu is a ring with the cursor on Resume: Options, Quit game, Start again, Resume, Score in most
# chapters, Options, Quit game, Resume in some (Venture, The Log). Seven UP presses reach Quit game in both.
QUIT_SEQUENCE = [(0.0, "START")] + [(1.5 + 0.5 * i, "UP") for i in range(7)] + [(5.5, "A"), (7.5, "A")]
# Yes goes straight back to the chapter select (no main menu), with the cursor on the chapter just played;
# it shows about 3 s after Yes.
QUIT_TO_SELECT = 13.0  # from START until the chapter select takes moves
SKIP_EVERY = 2.0  # SKIP (Start while a movie plays) presses
SKIP_FROM, SKIP_TO = 4.0, 40.0  # after the chapter's A


# ---------------------------------------------------------------------------
# Per chapter: the moment (seconds after the chapter's A) and what it shows. Filled in from the probes.
#   shots:  the test shot times (3-5)
#   burst:  golden-only frames around them (from, to, step); default shots[0]-1 .. shots[-1]+1, 0.2
#   stay:   when to quit (default last shot + 3)
#   skip:   (from, to) of the SKIP presses, or None for none
#   presses: extra presses, "t:BUTTONS[/hold]" relative to the chapter's A
#   ends:   the chapter returns to the chapter select by itself this long after its A (no quit)
#   kind:   gameplay, cutscene or menu; what: the picture
#   thresholds: overrides of TOUR_THRESHOLDS
def still(t: float, what: str, kind: str = "gameplay", **more) -> dict:
    """Five shots 0.5 s apart from t (seconds after the chapter's A)."""
    return dict(shots=[t, t + 0.5, t + 1.0, t + 1.5, t + 2.0], what=what, kind=kind, **more)


# Frame-to-frame motion of the whole picture (a raft on a river, Kong in a fight): accept what the golden burst
# showed anywhere near, and let two of five frames miss.
MOVING = {"env_radius": 4, "min_pass": 0.6}

MOMENTS: dict[int, dict] = {
    0: still(89.0, "the hull of the rocking ship with VENTURE painted on it and the Right stick to aim hint, "
             "after about 60 s of black (the same moment as the venture scene)",
             thresholds={"mae": 6.0, "ssim": 0.8, "tile_mae": 18.0, "drawlog": 0.97, "env_bad": 0.01}),
    1: still(28.0, "Ann's face leaning over Jack on the beach, the in-engine opening that waits for the player; "
             "birds cross the sky", kind="cutscene"),
    2: still(30.0, "a cave mouth against a pale sky, Ann and a sailor beside it (Jack still)"),
    3: still(30.0, "a dark stone passage with a carved wall (Jack still)"),
    4: still(30.0, "a jungle clearing at dusk, a spiked wooden gate against the sky (as chap04)"),
    5: still(40.0, "bones and torch glow on the floor of a dark pit (Jack still)"),
    6: still(40.0, "under a wooden scaffold in the rain, lightning lights it now and then (Jack still)"),
    7: still(40.0, "Hayes by a stone pillar under the scaffold, rain beyond (Jack still)"),
    8: still(40.0, "Jack in the rain under the cave roof, random lightning (as the vrex scenes)"),
    9: still(30.0, "a cave mouth with shafts of light falling in (Jack still)"),
    10: still(12.0, "Kong crouching on a ledge by a ruined wall (Kong idle; Ann is caught at about 24 s, so the "
              "shots come before that)"),
    11: still(30.0, "a canyon path between mossy rocks, a pool beyond (Jack still)"),
    12: still(30.0, "a dark jungle path towards a lit gate in a wall of vines (as chap12)"),
    13: still(30.0, "a dark cave, a thin stream of water falling from the roof (Jack still)"),
    14: still(20.0, "a dark rock wall lit by a torch, Carl with his camera at the start (Jack still)"),
    15: still(4.0, "the raft on the river at the start, a torch in the foreground (the raft moves)",
              thresholds=MOVING),
    16: still(6.0, "the raft going down the rapids in white water (moves; Jack drowns at about 20 s and the "
              "checkpoint restarts, so the shots come early)", thresholds=MOVING),
    17: still(6.0, "Kong and a V-Rex facing each other in a misty clearing (fight; Kong dies at about 50 s)",
              thresholds=MOVING),
    18: still(20.0, "Jack under a shelter, Jimmy and the sailors looking out over the swamp (Jack still)"),
    19: still(6.0, "Kong fighting raptors in the misty jungle, Ann behind him (Kong dies at about 25 s)",
              thresholds=MOVING),
    20: still(55.0, "after the log falls: a mossy rock wall over mist (as chap20)"),
    21: still(12.0, "Kong by a broken stone block in the ruins, Ann at his feet, raptors coming (Kong idle; "
              "he dies at about 40 s)", thresholds=MOVING),
    22: still(30.0, "a narrow rocky gorge with a stream, light at its end (Jack still)"),
    23: still(30.0, "a dark wooden scaffold over a pool, figures behind it (Jack still)"),
    24: still(30.0, "a rocky stream bed, two figures in front of a pale rock beyond (Jack still)"),
    25: still(30.0, "carved stone pillars in the dark, rain beyond (Jack still)"),
    26: still(30.0, "a dark hall with a lattice of beams over pale water (Jack still)"),
    27: still(13.0, "Kong to the Rescue in play: Jack with a spear behind the rocks, a V-Rex, fires and rain "
              "(the same moment as the kong scene)",
              thresholds={"env_tile": 6.0, "env_bad": 0.006, "drawlog": 0.95}),
    28: still(12.0, "Jack crouching by a body in a mossy stone corridor at the start (he dies at about 48 s)"),
    29: still(30.0, "a great tree's roots in green mist (Jack still)"),
    30: still(30.0, "dark carved stone walls in Kong's lair (Jack still)"),
    31: still(3.0, "Kong crouching in the lair as the giant snake comes at him, a waterfall behind (fight; "
              "Kong is hurt from about 6 s and dies at about 30 s)", thresholds=MOVING),
    32: still(30.0, "a very dark rock face, water dripping in front of it (Jack still)"),
    33: still(30.0, "a cave with a water channel, dark green rock (as chap33)"),
    34: still(30.0, "under a torch-lit wooden roof on stone posts, a dark gap beyond (Jack still)"),
    35: still(30.0, "worn stone steps going up through dark rock, water dripping (Jack still)"),
    36: still(30.0, "Kong from behind in a misty gorge, a wall of rock and roots beyond (Kong idle)"),
    37: still(30.0, "the Venture at anchor in the bay, seen from the rocks of the shore at dusk (Jack still)"),
    38: still(20.0, "Kong in chains on the dark theatre stage, before the lights come up at about 33 s",
              kind="cutscene"),
    39: still(30.0, "a New York rooftop at night, lit windows in the building beside it (Kong still)"),
    40: still(17.0, "the closing cutscene: Carl Denham standing by Kong's head in the street; it goes back to "
              "the chapter select by itself at about 57 s", kind="cutscene", skip=None, ends=60.0),
    41: still(3.0, "the credits over a misty mountain in the jungle, the title lines at the start (text "
              "scrolls)", kind="cutscene", skip=None, thresholds=MOVING),
}

# Thresholds of an animated gameplay or cutscene moment, as for the chap04-chap33 scenes; tuned per scene from
# the Xenos self-check (calibrate).
TOUR_THRESHOLDS = {
    "mae": None, "ssim": None, "tile_mae": None, "bad": None,
    "min_pass": 0.6, "env_tol": 32, "env_flash_level": 6,
}

# Launches: chapters played in one launch, in order. Every batch starts from the chapter select on entry 0.
# Four launches of about ten chapters (8 to 12 minutes each). A chapter that ends by itself (Kong's Death) is
# followed straight from the chapter select; Credits come last. Venture comes last in its launch: the quit
# failed there in the first golden capture (seven UP presses ended on Options), and The Log is last in its own.
BATCHES: list[list[int]] = [
    list(range(1, 11)) + [0],
    list(range(11, 21)),
    list(range(21, 31)),
    list(range(31, 42)),
]
# The scene whose frame log a batch takes (one window per launch, seconds after the chapter's A): calm
# chapters with Jack still.
FRAME_LOG: dict[int, float] = {4: 31.0, 18: 21.0, 22: 31.0, 33: 31.0}


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
        if skip and probe_stay is None and "shots" in m:
            # Not into the quit: the pause menu must open on the first START.
            skip = (skip[0], min(skip[1], m.get("stay", m["shots"][-1] + 3.0) - 1.0))
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
        if m.get("ends") is not None:
            # The chapter goes back to the chapter select by itself (a closing cutscene): no quit.
            t = start + m["ends"] + 3.0
        else:
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


def make_probe(chapters, every, stay, noskip=(), ends=None) -> dict:
    data = base_file()
    data["about"] = "A probe of the chapter tour (tests/tourgen.py --probe): shots every few seconds."
    data["defaults"]["startup_allowance"] = 120
    def moment(i):
        m = dict(MOMENTS.get(i, {}))
        if i in noskip:
            m["skip"] = None
        if ends and i in ends:
            m["ends"] = ends[i]
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
    p.add_argument("--ends", default="", help="probe: chapter:seconds for chapters that return by themselves")
    p.add_argument("--table", action="store_true", help="print the chapter table for docs/testing.md")
    p.add_argument("--check", action="store_true", help="print the batches and their timing")
    p.add_argument("--out", default=str(HERE / "tour.json"))
    a = p.parse_args(argv)
    if a.probe:
        chapters = [int(c) for c in a.chapters.split(",") if c.strip()]
        noskip = {int(c) for c in a.noskip.split(",") if c.strip()}
        ends = {int(k): float(v) for k, v in (e.split(":") for e in a.ends.split(",") if e.strip())}
        data = make_probe(chapters, a.every, a.stay, noskip, ends)
        path = Path(a.probe)
    else:
        missing = [i for i in range(42) if i not in MOMENTS]
        data = make_tour()
        path = Path(a.out)
        if missing:
            print(f"no moment yet for chapters {missing}", file=sys.stderr)
    if a.table:
        print("| Entry | Scene | Chapter | Run | Shots (s after A) | Kind | What |")
        print("|---|---|---|---|---|---|---|")
        for s in data["scenes"]:
            m = MOMENTS[s["chapter"]]
            shots = ", ".join(fmt(t) for t in m["shots"])
            print(f"| {s['chapter']} | `{s['name']}` | {CHAPTERS[s['chapter']]} | {s['run']} | {shots} | {m['kind']} | "
                  f"{m['what']} |")
        return 0
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
