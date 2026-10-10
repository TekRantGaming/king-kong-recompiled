#!/usr/bin/env python3
"""Golden-frame test harness for the King Kong native renderer: the Python half.

run.ps1 launches the game; this file plans the runs and does everything else:

  plan       what to launch for the chosen scenes (environment, arguments, expected shots), as JSON
  collect    after a run: copy its shots into per-scene folders, cut the frame log out of the game log
  compare    compare a capture with the golden set: per-frame scores, heat maps, report.html, summary.json
  calibrate  compare two captures of the same plugin and suggest thresholds per scene
  framelog   print a frame log in canonical form, or diff two of them

Run it with `python -I`. Needs numpy and Pillow. Exit codes: 0 pass, 1 fail, 2 missing data or bad input.
"""

from __future__ import annotations

import argparse
import datetime as dt
import difflib
import html
import json
import math
import os
import re
import shutil
import statistics
import sys
from collections import Counter
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
DEFAULT_SCENES = HERE / "scenes.json"

# ---------------------------------------------------------------------------
# Scene definitions


def load_scenes(path: Path) -> dict:
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    names = set()
    for s in data["scenes"]:
        if s["name"] in names:
            raise SystemExit(f"scenes: duplicate scene {s['name']}")
        names.add(s["name"])
        if s["run"] not in data["runs"]:
            raise SystemExit(f"scenes: {s['name']} uses unknown run {s['run']}")
        s["golden_times"] = expand_times(s.get("golden_times", []))
        for t in list(s["times"]) + s["golden_times"]:
            if t <= 0 or abs(round(t * 10) - t * 10) > 1e-6:
                raise SystemExit(f"scenes: {s['name']} time {t} is not a positive multiple of 0.1 s")
    return data


def expand_times(spec) -> list[float]:
    """A list of seconds, or {"from", "to", "step"} for an evenly spaced burst."""
    if isinstance(spec, dict):
        n = int(round((spec["to"] - spec["from"]) / spec["step"]))
        return [round(spec["from"] + i * spec["step"], 1) for i in range(n + 1)]
    return [float(t) for t in spec]


def all_times(scene: dict, golden: bool) -> list[float]:
    """The scene's test times, plus its extra golden-only times when capturing the golden set."""
    ts = {round(float(t), 1) for t in scene["times"]}
    if golden:
        ts |= {round(float(t), 1) for t in scene["golden_times"]}
    return sorted(ts)


def pick_scenes(data: dict, which: str) -> list[dict]:
    if not which or which == "all":
        return list(data["scenes"])
    wanted = [w.strip() for w in which.split(",") if w.strip()]
    by_name = {s["name"]: s for s in data["scenes"]}
    out = []
    for w in wanted:
        if w in by_name:
            out.append(by_name[w])
        elif w in data["runs"]:  # a run name selects all its scenes
            out.extend(s for s in data["scenes"] if s["run"] == w and s not in out)
        else:
            raise SystemExit(f"unknown scene or run '{w}' (known: {', '.join(by_name)})")
    return out


def time_text(t: float) -> str:
    """How a time is written into KK_DEV_SHOTS (one decimal at most)."""
    return f"{float(t):.1f}".rstrip("0").rstrip(".")


def shot_name(t: float) -> str:
    # dev_tools.cpp parses the text with std::stod and names the file "shot_" + int(seconds * 10) + ".bmp".
    return f"shot_{int(float(time_text(t)) * 10)}.bmp"


def thresholds_for(data: dict, scene: dict) -> dict:
    th = dict(data["defaults"]["thresholds"])
    th.update(scene.get("thresholds", {}))
    return th


# ---------------------------------------------------------------------------
# plan


def cmd_plan(a) -> int:
    data = load_scenes(Path(a.scenes_file))
    scenes = pick_scenes(data, a.scenes)
    d = data["defaults"]
    runs = []
    for run_name, run in data["runs"].items():
        mine = [s for s in scenes if s["run"] == run_name]
        if not mine:
            continue
        times = sorted({t for s in mine for t in all_times(s, a.golden)})
        env = dict(d.get("env", {}))
        env.update(run.get("env", {}))
        env["KK_DEV_SHOTS"] = ",".join(time_text(t) for t in times)
        env["KK_DEV_SHOTS_FROM"] = run["from"]
        env["KK_DEV_SCRIPT"] = run.get("script", "")
        fl = run.get("frame_log")
        frames = 0
        if fl and any(s["name"] == fl["scene"] for s in mine) and not a.no_frame_log:
            scene = next(s for s in mine if s["name"] == fl["scene"])
            at = float(fl.get("at", statistics.median(scene["times"])))
            offset = measured_offset(Path(a.golden_root), run_name) if a.golden_root else None
            if offset is None:
                offset = float(run.get("clock_offset", d.get("clock_offset", {}).get(run["from"], 0.0)))
            frames = int(fl.get("frames", 2))
            env["REX_DEV_FRAME_LOG"] = f"{max(0.0, at + offset):.2f},{frames}"
        else:
            env["REX_DEV_FRAME_LOG"] = ""
        args = list(d.get("args", [])) + list(run.get("args", []))
        if a.plugin:
            args.append(f"--gpu_plugin={a.plugin}")
        startup = float(d.get("startup_allowance", 60))
        runs.append({
            "name": run_name,
            "from": run["from"],
            "env": env,
            "args": args,
            "expected": [shot_name(t) for t in times],
            "frame_log_frames": frames,
            "limit_s": round(max(times) + startup + float(run.get("extra_time", 0)), 1),
            "scenes": [s["name"] for s in mine],
        })
    json.dump({"runs": runs}, sys.stdout, indent=1)
    sys.stdout.write("\n")
    return 0


def measured_offset(golden_root: Path, run_name: str):
    """The frame-log clock offset measured in the golden run, if there is one."""
    meta = golden_root / "_runs" / f"{run_name}.json"
    try:
        with open(meta, encoding="utf-8") as f:
            v = json.load(f).get("frame_log", {}).get("clock_offset")
        return float(v) if v is not None else None
    except (OSError, ValueError):
        return None


# ---------------------------------------------------------------------------
# Game log parsing

LOG_LINE = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\] \[(\w+)\] \[(\w+)\] \[t\d+\] (.*)$")


def read_log(path: Path):
    """Yields (timestamp seconds, level, category, message) for each log line."""
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LOG_LINE.match(line.rstrip("\n"))
            if not m:
                continue
            ts = dt.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp()
            yield ts, m.group(2), m.group(3), m.group(4)


def frame_log_blocks(messages):
    """Splits "Frame log:" messages into frames: a list of (start timestamp, [lines]).

    The log writes "frame start" once, then "frame end, ..." after every logged frame; the frames after the
    first have no start line of their own. A frame without its end line (log cut short) is dropped."""
    frames, cur, start = [], None, 0.0
    for ts, _lvl, _cat, msg in messages:
        if not msg.startswith("Frame log: "):
            continue
        body = msg[len("Frame log: "):]
        if body == "frame start":
            cur, start = [], ts
        elif body.startswith("frame end"):
            if cur is not None:
                frames.append((start, cur + [body]))
            cur, start = [], ts  # the next frame, if the log goes on
        elif cur is not None:
            cur.append(body)
    return frames


# ---------------------------------------------------------------------------
# collect


def cmd_collect(a) -> int:
    data = load_scenes(Path(a.scenes_file))
    scenes = [s for s in pick_scenes(data, a.scenes) if s["run"] == a.run]
    run = data["runs"][a.run]
    run_dir, out_root = Path(a.run_dir), Path(a.out_root)
    log_path = run_dir / "game.log"
    messages = list(read_log(log_path)) if log_path.exists() else []
    saved = {}
    menu_ts = None
    warnings = []
    for ts, lvl, _cat, msg in messages:
        m = re.match(r"KK: saved frame (shot_\d+\.bmp) \((\d+)x(\d+)\)", msg)
        if m:
            saved[m.group(1)] = (ts, int(m.group(2)), int(m.group(3)))
        elif msg == "KK: save menu opened" and menu_ts is None:
            menu_ts = ts
        elif lvl in ("error", "critical") and "ResolvePath" not in msg:
            warnings.append(msg[:200])
    # The shot clock's origin (log time of shot second 0): a shot is saved a little after its time.
    origins = []
    for s in scenes:
        for t in all_times(s, a.golden):
            n = shot_name(t)
            if n in saved:
                origins.append(saved[n][0] - float(t))
    origin = min(origins) if origins else None
    blocks = frame_log_blocks(messages)
    trigger = None
    if a.trigger:
        trigger = float(a.trigger.split(",")[0])
    fl_meta = {}
    if blocks:
        first_ts = blocks[0][0]
        fl_meta = {"frames": len(blocks), "trigger": trigger}
        if origin is not None:
            fl_meta["scene_time"] = round(first_ts - origin, 2)
            if trigger is not None:
                # frame-log clock starts at about first_ts - trigger; offset = shot origin - that start
                fl_meta["clock_offset"] = round(origin - (first_ts - trigger), 2)
    missing = []
    for s in scenes:
        dest = out_root / s["name"]
        dest.mkdir(parents=True, exist_ok=True)
        got = []
        for t in all_times(s, a.golden):
            n = shot_name(t)
            src = run_dir / n
            if src.exists():
                shutil.copyfile(src, dest / n)
                got.append(n)
            else:
                missing.append(f"{s['name']}/{n}")
        meta = {
            "scene": s["name"],
            "run": a.run,
            "plugin": a.plugin,
            "label": a.label,
            "captured": dt.datetime.now().isoformat(timespec="seconds"),
            "golden": bool(a.golden),
            "times": all_times(s, a.golden),
            "shots": got,
            "sizes": sorted({f"{saved[n][1]}x{saved[n][2]}" for n in got if n in saved}),
            "run_dir": str(run_dir),
        }
        fl = run.get("frame_log")
        if fl and fl["scene"] == s["name"] and blocks:
            with open(dest / "framelog.txt", "w", encoding="utf-8", newline="\n") as f:
                for i, (_ts, lines) in enumerate(blocks):
                    f.write(f"frame {i}\n")
                    for line in lines:
                        f.write(line + "\n")
            meta["frame_log"] = fl_meta
        with open(dest / "meta.json", "w", encoding="utf-8") as f:
            json.dump(meta, f, indent=1)
    runs_dir = out_root / "_runs"
    runs_dir.mkdir(parents=True, exist_ok=True)
    run_meta = {
        "run": a.run,
        "plugin": a.plugin,
        "label": a.label,
        "run_dir": str(run_dir),
        "menu_after_shot_origin": round(menu_ts - origin, 2) if (menu_ts and origin) else None,
        "frame_log": fl_meta,
        "missing": missing,
        "errors_in_log": warnings[:20],
    }
    with open(runs_dir / f"{a.run}.json", "w", encoding="utf-8") as f:
        json.dump(run_meta, f, indent=1)
    print(json.dumps({"run": a.run, "copied_scenes": [s["name"] for s in scenes], "missing": missing,
                      "frame_log": fl_meta}))
    return 2 if missing else 0


# ---------------------------------------------------------------------------
# Images


def load_image(path: Path) -> np.ndarray:
    """RGB uint8 (H, W, 3). Reads the game's 32-bit BMPs directly; anything else through Pillow."""
    raw = path.read_bytes()
    if raw[:2] == b"BM" and len(raw) >= 54:
        off, = np.frombuffer(raw, "<u4", 1, 10)
        w, h = np.frombuffer(raw, "<i4", 2, 18)
        bpp, = np.frombuffer(raw, "<u2", 1, 28)
        comp, = np.frombuffer(raw, "<u4", 1, 30)
        if comp in (0, 3) and bpp in (24, 32):
            rows, stride = abs(int(h)), ((int(w) * int(bpp) + 31) // 32) * 4
            px = np.frombuffer(raw, np.uint8, rows * stride, int(off)).reshape(rows, stride)
            px = px[:, : int(w) * (bpp // 8)].reshape(rows, int(w), bpp // 8)[..., 2::-1]
            if h > 0:
                px = px[::-1]
            return np.ascontiguousarray(px)
    with Image.open(path) as im:
        return np.asarray(im.convert("RGB"))


def box_mean(x: np.ndarray, k: int) -> np.ndarray:
    c = np.zeros((x.shape[0] + 1, x.shape[1] + 1), np.float64)
    c[1:, 1:] = x.cumsum(0).cumsum(1)
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


def ssim(a: np.ndarray, b: np.ndarray, k: int = 8) -> float:
    """Mean SSIM on luma with k x k box windows (the usual constants for 8-bit data)."""
    wts = np.array([0.299, 0.587, 0.114])
    ya, yb = a.astype(np.float64) @ wts, b.astype(np.float64) @ wts
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    ma, mb = box_mean(ya, k), box_mean(yb, k)
    va = box_mean(ya * ya, k) - ma * ma
    vb = box_mean(yb * yb, k) - mb * mb
    cov = box_mean(ya * yb, k) - ma * mb
    m = ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2))
    return float(m.mean())


def tile_mae(d: np.ndarray, tiles=(16, 9)) -> float:
    """The worst mean absolute error over a grid of tiles (catches small missing or wrong objects)."""
    h, w = d.shape[:2]
    tx, ty = tiles
    th, tw = h // ty, w // tx
    t = d[: th * ty, : tw * tx].astype(np.float64).mean(axis=2)
    t = t.reshape(ty, th, tx, tw).mean(axis=(1, 3))
    return float(t.max())


def metrics(g: np.ndarray, t: np.ndarray, bad_level: int) -> dict:
    d = np.abs(g.astype(np.int16) - t.astype(np.int16)).astype(np.uint8)
    dmax = d.max(axis=2)
    mse = float((d.astype(np.float64) ** 2).mean())
    return {
        "mae": round(float(d.mean()), 3),
        "psnr": round(10 * math.log10(255 * 255 / mse), 2) if mse > 0 else 99.0,
        "ssim": round(ssim(g, t), 5),
        "tile_mae": round(tile_mae(d), 2),
        "bad": round(float((dmax > bad_level).mean()), 5),
        "_dmax": dmax,
    }


def envelope_metrics(gmin: np.ndarray, gmax: np.ndarray, t: np.ndarray, tol: int) -> dict:
    """How far the test frame leaves the golden range (the per-pixel span of the scene's golden frames).

    Animated parts (videos, rain, a blinking prompt) have a wide range and accept any value in it; still
    parts have a range of zero and must match. Distances are per pixel, the largest over the channels."""
    ti = t.astype(np.int16)
    below = gmin.astype(np.int16) - ti
    above = ti - gmax.astype(np.int16)
    dist = np.maximum(np.maximum(below, above), 0).max(axis=2).astype(np.uint8)
    return {
        "env_mae": round(float(dist.mean()), 3),
        "env_bad": round(float((dist > tol).mean()), 5),
        "env_tile": round(tile_mae(dist[..., None]), 2),
        "_dist": dist,
    }


def quick_mae(g: np.ndarray, t: np.ndarray) -> float:
    return float(np.abs(g[::4, ::4].astype(np.int16) - t[::4, ::4].astype(np.int16)).mean())


_HEAT = None


def heat_lut() -> np.ndarray:
    global _HEAT
    if _HEAT is None:
        stops = [0.0, 0.2, 0.45, 0.7, 1.0]
        cols = np.array([(0, 0, 0), (70, 0, 130), (210, 30, 50), (250, 170, 0), (255, 255, 230)], np.float64)
        x = np.linspace(0, 1, 256)
        _HEAT = np.stack([np.interp(x, stops, cols[:, i]) for i in range(3)], axis=1).astype(np.uint8)
    return _HEAT


def heat_image(g: np.ndarray, dmax: np.ndarray, full_at: int = 64) -> Image.Image:
    """Difference heat map over a dimmed grey copy of the golden frame."""
    v = np.clip(dmax.astype(np.float64) / full_at, 0, 1) ** 0.5
    heat = heat_lut()[(v * 255).astype(np.uint8)].astype(np.float64)
    grey = (g.astype(np.float64) @ np.array([0.299, 0.587, 0.114]))[..., None] * 0.3
    out = np.clip(heat + grey * (1 - v)[..., None], 0, 255).astype(np.uint8)
    return Image.fromarray(out)


def save_thumb(img, path: Path, width: int):
    im = img if isinstance(img, Image.Image) else Image.fromarray(img)
    h = round(im.height * width / im.width)
    im = im.resize((width, h), Image.Resampling.BOX)
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.suffix == ".jpg":
        im.save(path, quality=88)
    else:
        im.save(path)


# ---------------------------------------------------------------------------
# Frame logs (REX_DEV_FRAME_LOG format; see docs/testing.md)

DRAW_RE = re.compile(
    r"draw (\d+) ps ([0-9A-Fa-f]{16}) vs ([0-9A-Fa-f]{16}) pitch (\d+) msaa (\d+) z (\S+)(?: func (\d+))? "
    r"@(\d+):f(\d+) off (-?\d+),(-?\d+) scissor (\d+)x(\d+)((?: rt\d+@\d+:f\d+)*) count (\d+)$")
RESOLVE_RE = re.compile(
    r"resolve (\d+) (rt\d+|depth)@(\d+):f(\d+) -> ([0-9A-Fa-f]{8}) format (\d+) pitch (\d+) height (\d+) "
    r"command (\d+) clear color (\d+) depth (\d+) off (-?\d+),(-?\d+) surface pitch (\d+) msaa (\d+)$")
RT_RE = re.compile(r"rt(\d+)@(\d+):f(\d+)")


def parse_entry(line: str):
    """One frame-log line -> (loose key, strict key, kind), or None for lines that are neither."""
    m = DRAW_RE.match(line)
    if m:
        (_n, ps, vs, pitch, msaa, z, func, dbase, dfmt, ox, oy, sw, sh, rts, count) = m.groups()
        rtl = RT_RE.findall(rts)
        fmts = ",".join(f"{i}:f{f}" for i, _b, f in rtl) or "-"
        ds = f"f{dfmt}" if z != "off" else "-"
        loose = f"draw ps={ps.upper()} vs={vs.upper()} z={z} msaa={msaa} scissor={sw}x{sh} rt={fmts} ds={ds} n={count}"
        bases = ",".join(f"{i}@{b}" for i, b, _f in rtl) or "-"
        strict = loose + f" func={func or '?'} pitch={pitch} rtbase={bases} dsbase={dbase} off={ox},{oy}"
        return loose, strict, "draw"
    m = RESOLVE_RE.match(line)
    if m:
        (_n, src, sbase, sfmt, dest, dfmt, pitch, height, cmd, cc, cd, ox, oy, spitch, msaa) = m.groups()
        loose = (f"resolve src={src}:f{sfmt} format={dfmt} size={pitch}x{height} command={cmd} "
                 f"clear={cc}{cd}")
        strict = loose + f" dest={dest.upper()} srcbase={sbase} off={ox},{oy} spitch={spitch} msaa={msaa}"
        return loose, strict, "resolve"
    return None


def read_framelog(path: Path):
    """framelog.txt (written by collect) or a whole game log -> list of frames, each a list of lines."""
    text = path.read_text(encoding="utf-8", errors="replace")
    if "Frame log: " in text:
        return [lines for _ts, lines in frame_log_blocks(read_log(path))]
    frames, cur = [], None
    for line in text.splitlines():
        if re.fullmatch(r"frame \d+", line):
            cur = []
            frames.append(cur)
        elif cur is not None:
            cur.append(line)
    return frames


def frame_keys(lines, strict=False):
    keys = []
    for line in lines:
        p = parse_entry(line)
        if p:
            keys.append(p[1] if strict else p[0])
    return keys


def compare_framelogs(golden_frames, test_frames, strict=False) -> dict:
    """For each test frame, the most similar golden frame (difflib ratio over the canonical lines)."""
    best_all = []
    for ti, tl in enumerate(test_frames):
        tk = frame_keys(tl, strict)
        best = None
        for gi, gl in enumerate(golden_frames):
            gk = frame_keys(gl, strict)
            sm = difflib.SequenceMatcher(None, gk, tk, autojunk=False)
            r = sm.ratio() if (gk or tk) else 1.0
            if best is None or r > best["ratio"]:
                best = {"test_frame": ti, "golden_frame": gi, "ratio": round(r, 4), "_g": gk, "_t": tk}
        if best:
            best_all.append(best)
    if not best_all:
        return {}
    worst = min(best_all, key=lambda b: b["ratio"])
    gk, tk = worst["_g"], worst["_t"]

    def pairs(keys):
        return Counter(" ".join(k.split()[:3]) for k in keys if k.startswith("draw"))

    gp, tp = pairs(gk), pairs(tk)
    out = {
        "ratio": worst["ratio"],
        "ratios": [b["ratio"] for b in best_all],
        "golden_frame": worst["golden_frame"],
        "test_frame": worst["test_frame"],
        "golden_draws": sum(1 for k in gk if k.startswith("draw")),
        "test_draws": sum(1 for k in tk if k.startswith("draw")),
        "golden_resolves": sum(1 for k in gk if k.startswith("resolve")),
        "test_resolves": sum(1 for k in tk if k.startswith("resolve")),
        "only_golden": [f"{n}x {p}" for p, n in (gp - tp).most_common(12)],
        "only_test": [f"{n}x {p}" for p, n in (tp - gp).most_common(12)],
        "diff": list(difflib.unified_diff(gk, tk, "golden", "test", n=2, lineterm="")),
    }
    return out


def cmd_framelog(a) -> int:
    fa = read_framelog(Path(a.log))
    if not a.other:
        for i, lines in enumerate(fa):
            print(f"# frame {i}")
            for k in frame_keys(lines, a.strict):
                print(k)
        return 0
    fb = read_framelog(Path(a.other))
    r = compare_framelogs(fa, fb, a.strict)
    if not r:
        print("no frames to compare")
        return 2
    print(f"similarity {r['ratio']:.4f} (golden frame {r['golden_frame']} vs test frame {r['test_frame']}); "
          f"draws {r['golden_draws']} -> {r['test_draws']}, resolves {r['golden_resolves']} -> {r['test_resolves']}")
    for line in r["diff"]:
        print(line)
    return 0


# ---------------------------------------------------------------------------
# compare / calibrate


def scene_shots(folder: Path, times):
    return [(float(t), folder / shot_name(t)) for t in times]


def compare_scene(data: dict, scene: dict, golden_root: Path, test_root: Path, out_dir: Path | None,
                  thumbs: bool) -> dict:
    th = thresholds_for(data, scene)
    gdir, tdir = golden_root / scene["name"], test_root / scene["name"]
    res = {"scene": scene["name"], "description": scene.get("description", ""), "thresholds": th,
           "frames": [], "notes": [], "status": "pass"}
    gshots = [(t, p) for t, p in scene_shots(gdir, all_times(scene, True)) if p.exists()]
    tshots = [(t, p) for t, p in scene_shots(tdir, scene["times"]) if p.exists()]
    if not gshots:
        res["status"] = "missing"
        res["notes"].append(f"no golden frames in {gdir}")
        return res
    if not tshots:
        res["status"] = "missing"
        res["notes"].append(f"no test frames in {tdir}")
        return res
    if len(tshots) < len(scene["times"]):
        res["notes"].append(f"{len(scene['times']) - len(tshots)} test frame(s) missing")
    golden = [(t, p, load_image(p)) for t, p in gshots]
    gshape = golden[0][2].shape
    golden = [g for g in golden if g[2].shape == gshape]
    # The golden range: per pixel and channel, the lowest and highest value over all golden frames of the scene.
    gmin, gmax = golden[0][2].copy(), golden[0][2].copy()
    for _t, _p, g in golden[1:]:
        np.minimum(gmin, g, out=gmin)
        np.maximum(gmax, g, out=gmax)
    bad_level = int(th.get("bad_level", 40))
    env_tol = int(th.get("env_tol", 16))
    for t, tp in tshots:
        timg = load_image(tp)
        note = ""
        if timg.shape != gshape:
            note = f"size {timg.shape[1]}x{timg.shape[0]}, golden {gshape[1]}x{gshape[0]}"
            timg = np.asarray(Image.fromarray(timg).resize((gshape[1], gshape[0]), Image.Resampling.BOX))
        # Timing between runs is never exact: match each test frame with the closest golden frame of the scene.
        gt, gp, gimg = min(golden, key=lambda g: quick_mae(g[2], timg))
        m = metrics(gimg, timg, bad_level)
        dmax = m.pop("_dmax")
        env = envelope_metrics(gmin, gmax, timg, env_tol)
        edist = env.pop("_dist")
        m.update(env)
        fails = [k for k in ("mae", "tile_mae", "bad", "env_bad", "env_tile") if th.get(k) is not None and m[k] > th[k]]
        if th.get("ssim") is not None and m["ssim"] < th["ssim"]:
            fails.append("ssim")
        if note:
            fails.append("size")
        fr = {"time": t, "golden_time": gt, "test": str(tp), "golden": str(gp), "fails": fails, "note": note, **m}
        if out_dir is not None and thumbs:
            stem = shot_name(t)[:-4]
            img = out_dir / "img" / scene["name"]
            save_thumb(gimg, img / f"{stem}_golden.jpg", 640)
            save_thumb(timg, img / f"{stem}_test.jpg", 640)
            save_thumb(heat_image(gimg, dmax), img / f"{stem}_diff.png", 640)
            save_thumb(heat_image(gimg, edist), img / f"{stem}_range.png", 640)
            fr["img"] = {k: f"img/{scene['name']}/{stem}_{k}.{'jpg' if k in ('golden', 'test') else 'png'}"
                         for k in ("golden", "test", "diff", "range")}
        res["frames"].append(fr)
    res["golden_frames"] = len(golden)
    # Scenes with random events (lightning, rain) may let a share of their frames miss.
    passed = sum(1 for f in res["frames"] if not f["fails"])
    need = math.ceil(float(th.get("min_pass", 1.0)) * len(scene["times"]) - 1e-9)
    res["passed_frames"], res["needed_frames"] = passed, need
    if passed < need or any("size" in f["fails"] for f in res["frames"]):
        res["status"] = "fail"
    # Draw lists
    gfl, tfl = gdir / "framelog.txt", tdir / "framelog.txt"
    if gfl.exists() and tfl.exists():
        dl = compare_framelogs(read_framelog(gfl), read_framelog(tfl), strict=bool(th.get("drawlog_strict")))
        if dl:
            res["drawlog"] = {k: v for k, v in dl.items() if k != "diff"}
            if out_dir is not None:
                p = out_dir / "drawlog" / f"{scene['name']}.diff.txt"
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text("\n".join(dl["diff"]) + "\n", encoding="utf-8")
                res["drawlog"]["diff_file"] = f"drawlog/{scene['name']}.diff.txt"
            if th.get("drawlog") is not None and dl["ratio"] < th["drawlog"]:
                res["status"] = "fail"
                res["drawlog"]["fail"] = True
        gm, tm = read_meta(gdir), read_meta(tdir)
        for who, mm in (("golden", gm), ("test", tm)):
            st = mm.get("frame_log", {}).get("scene_time")
            if st is not None:
                res["notes"].append(f"{who} frame log taken at scene time {st} s")
    elif gfl.exists():
        res["notes"].append("the test run has no frame log for this scene")
    return res


def read_meta(folder: Path) -> dict:
    try:
        with open(folder / "meta.json", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def cmd_compare(a) -> int:
    data = load_scenes(Path(a.scenes_file))
    scenes = pick_scenes(data, a.scenes)
    golden_root, test_root = Path(a.golden_root), Path(a.test_root)
    out_dir = Path(a.report_dir) if a.report_dir else test_root / "report"
    out_dir.mkdir(parents=True, exist_ok=True)
    results = []
    for s in scenes:
        r = compare_scene(data, s, golden_root, test_root, out_dir, thumbs=True)
        results.append(r)
        fr = r["frames"]
        print(f"{r['status']:8} {s['name']:16} frames {r.get('passed_frames', 0)}/{len(fr)} (need "
              f"{r.get('needed_frames', '-')}), worst: outside range {scene_worst(fr, 'env_bad') or 0:.4f}, "
              f"mae {scene_worst(fr, 'mae') or 0:.3f}"
              + (f", draw list {r['drawlog']['ratio']:.3f}" if "drawlog" in r else ""))
    status = "pass" if all(r["status"] == "pass" for r in results) else (
        "missing" if any(r["status"] == "missing" for r in results) else "fail")
    summary = {"status": status, "plugin": a.plugin, "golden_root": str(golden_root), "test_root": str(test_root),
               "created": dt.datetime.now().isoformat(timespec="seconds"), "scenes": results}
    with open(out_dir / "summary.json", "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1)
    (out_dir / "report.html").write_text(render_report(summary), encoding="utf-8")
    print(f"{status.upper()}: {sum(r['status'] == 'pass' for r in results)}/{len(results)} scenes pass; "
          f"report {out_dir / 'report.html'}")
    return {"pass": 0, "fail": 1}.get(status, 2)


def cmd_calibrate(a) -> int:
    """Compares two captures of the same plugin and suggests thresholds with a safety margin."""
    data = load_scenes(Path(a.scenes_file))
    scenes = pick_scenes(data, a.scenes)
    sugg = {}
    print("Worst value over each scene's frames, the k-th best where the scene lets some frames miss (min_pass).")
    print(f"{'scene':16} {'mae':>7} {'ssim':>8} {'tile':>7} {'bad':>8} {'env_bad':>8} {'env_tile':>8} {'drawlog':>8}")
    for s in scenes:
        r = compare_scene(data, s, Path(a.golden_root), Path(a.test_root), None, thumbs=False)
        if not r["frames"]:
            print(f"{s['name']:16} {r['status']} {'; '.join(r['notes'])}")
            continue
        need = max(1, r.get("needed_frames", len(r["frames"])))

        def kth(key, higher_is_worse=True):
            vals = sorted((f[key] for f in r["frames"]), reverse=not higher_is_worse)
            return vals[min(need, len(vals)) - 1]

        mae, ss, tile, bad = kth("mae"), kth("ssim", False), kth("tile_mae"), kth("bad")
        eb, et = kth("env_bad"), kth("env_tile")
        dl = r.get("drawlog", {}).get("ratio")
        print(f"{s['name']:16} {mae:7.3f} {ss:8.5f} {tile:7.2f} {bad:8.5f} {eb:8.5f} {et:8.2f} "
              f"{dl if dl is not None else '-':>8}")
        f = a.margin
        sugg[s["name"]] = {
            "mae": round(max(mae * f, a.floor_mae), 2),
            "ssim": round(min(1 - (1 - ss) * f, 1 - a.floor_ssim), 4),
            "tile_mae": round(max(tile * f, a.floor_tile), 1),
            "bad": round(max(bad * f, a.floor_bad), 4),
            "env_bad": round(max(eb * f, a.floor_bad), 4),
            "env_tile": round(max(et * f, a.floor_env_tile), 2),
        }
        if dl is not None:
            sugg[s["name"]]["drawlog"] = round(max(0.0, min(1 - (1 - dl) * f, 1 - a.floor_drawlog)), 3)
    print(json.dumps(sugg, indent=1))
    return 0


# ---------------------------------------------------------------------------
# report.html

CSS = """
:root{--bg:#f6f6f4;--fg:#1d1d1f;--muted:#6b6b70;--card:#fff;--line:#e2e2e0;--pass:#1f7a3a;--passbg:#e3f3e7;
--fail:#b3261e;--failbg:#fbe4e2;--miss:#7a5a00;--missbg:#fdf1d0;--accent:#2f5bd3}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){--bg:#141416;--fg:#ececef;--muted:#9a9aa2;
--card:#1d1d21;--line:#2e2e34;--pass:#6fd08c;--passbg:#173322;--fail:#ff8a80;--failbg:#3a1a18;--miss:#f0c75e;
--missbg:#3a3014;--accent:#8aa8ff}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,Segoe UI,sans-serif}
main{max-width:1500px;margin:0 auto;padding:20px 16px 60px}h1{font-size:22px;margin:0 0 4px}
.sub{color:var(--muted);margin-bottom:16px;word-break:break-all}
.badge{display:inline-block;padding:2px 10px;border-radius:999px;font-weight:600;font-size:12px;letter-spacing:.03em}
.pass{color:var(--pass);background:var(--passbg)}.fail{color:var(--fail);background:var(--failbg)}
.missing{color:var(--miss);background:var(--missbg)}
.big{font-size:18px;padding:6px 16px;margin-right:10px}
table{border-collapse:collapse;width:100%;background:var(--card);border:1px solid var(--line);border-radius:8px;overflow:hidden}
th,td{padding:6px 10px;border-bottom:1px solid var(--line);text-align:right;white-space:nowrap}
th:first-child,td:first-child,td.l{text-align:left}th{font-weight:600;color:var(--muted);font-size:12px}
td.bad{color:var(--fail);font-weight:600}.wrap{overflow-x:auto}
details{background:var(--card);border:1px solid var(--line);border-radius:8px;margin:14px 0;padding:0 14px}
summary{cursor:pointer;padding:12px 0;font-weight:600}summary .badge{margin-left:8px}
.desc{color:var(--muted);margin:0 0 10px}.frames{display:grid;gap:14px;margin-bottom:14px}
.frame{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:8px}.lim{color:var(--muted);font-weight:400}
.frame figure{margin:0}.frame img{width:100%;display:block;border-radius:4px;border:1px solid var(--line)}
figcaption{font-size:12px;color:var(--muted)}.m{font-size:12px;margin:2px 0 0;color:var(--fg)}
.m b.bad{color:var(--fail)}ul{margin:4px 0 12px;padding-left:18px}code{font-size:12px}
a{color:var(--accent)}
@media (max-width:1000px){.frame{grid-template-columns:repeat(2,minmax(0,1fr))}}
@media (max-width:560px){.frame{grid-template-columns:1fr}}
"""


def fmt_cell(value, limit, higher_is_bad=True, digits=3, scale=1.0):
    """A table cell: the value, the scene's limit beside it, red when over. No limit = not checked."""
    if value is None:
        return "<td>-</td>"
    value *= scale
    if limit is None:
        return f"<td>{value:.{digits}f} <span class=lim>/ -</span></td>"
    limit *= scale
    bad = value > limit + 1e-12 if higher_is_bad else value < limit - 1e-12
    return f"<td class=\"{'bad' if bad else ''}\">{value:.{digits}f} <span class=lim>/ {limit:g}</span></td>"


def scene_worst(frames, key, higher_is_worse=True):
    vals = [f[key] for f in frames]
    if not vals:
        return None
    return max(vals) if higher_is_worse else min(vals)


def render_report(summary: dict) -> str:
    e = html.escape
    rs = summary["scenes"]
    npass = sum(r["status"] == "pass" for r in rs)
    out = ["<!doctype html><html lang=en><head><meta charset=utf-8>",
           "<meta name=viewport content=\"width=device-width,initial-scale=1\">",
           f"<title>Renderer test report</title><style>{CSS}</style></head><body><main>",
           "<h1>Renderer test report</h1>",
           f"<div class=sub>plugin <b>{e(str(summary.get('plugin') or '?'))}</b> against the golden set "
           f"{e(summary['golden_root'])}<br>test capture {e(summary['test_root'])} &middot; {e(summary['created'])}</div>",
           f"<p><span class=\"badge big {summary['status']}\">{summary['status'].upper()}</span>"
           f"{npass} of {len(rs)} scenes pass</p>",
           "<div class=wrap><table><tr><th>Scene</th><th>Result</th><th>Frames passing / needed</th>"
           "<th>Outside golden range %</th><th>Worst tile outside</th><th>Worst MAE</th><th>Lowest SSIM</th>"
           "<th>Worst tile MAE</th><th>Bad pixels %</th><th>Draw list</th></tr>"]
    for r in rs:
        th, fr = r["thresholds"], r["frames"]
        dl = r.get("drawlog", {}).get("ratio")
        out.append(f"<tr><td class=l><a href=\"#{e(r['scene'])}\">{e(r['scene'])}</a></td>"
                   f"<td><span class=\"badge {r['status']}\">{r['status']}</span></td>"
                   f"<td>{r.get('passed_frames', 0)} of {len(fr)} <span class=lim>/ {r.get('needed_frames', '-')}</span></td>"
                   + fmt_cell(scene_worst(fr, "env_bad"), th.get("env_bad"), True, 3, 100)
                   + fmt_cell(scene_worst(fr, "env_tile"), th.get("env_tile"), True, 2)
                   + fmt_cell(scene_worst(fr, "mae"), th.get("mae"), True, 3)
                   + fmt_cell(scene_worst(fr, "ssim", False), th.get("ssim"), False, 4)
                   + fmt_cell(scene_worst(fr, "tile_mae"), th.get("tile_mae"), True, 1)
                   + fmt_cell(scene_worst(fr, "bad"), th.get("bad"), True, 3, 100)
                   + (fmt_cell(dl, th.get("drawlog"), False, 3) if dl is not None else "<td>-</td>") + "</tr>")
    out.append("</table></div>")
    out.append("<p class=desc>Each value is the worst over the scene's test frames, with the scene's limit after the "
               "slash (- = not checked). <b>Outside golden range</b>: share of pixels that leave the span of values "
               "the golden frames of the scene show at that pixel by more than the scene's tolerance (animated parts "
               "have a wide span, still parts none). <b>Worst tile outside</b>: the worst 80x80 tile of that distance. "
               "<b>MAE</b>, <b>SSIM</b>, <b>tile MAE</b> and <b>bad pixels</b> compare with the closest golden frame "
               "(mean difference per channel, 0-255; structural similarity of brightness, 1 = same; the worst 80x80 "
               "tile; pixels off by more than the bad level). <b>Draw list</b>: similarity of the frame logs' draw and "
               "resolve lines, 1 = the same list. A scene passes when enough of its frames pass every check.</p>")
    for r in rs:
        th = r["thresholds"]
        open_ = "" if r["status"] == "pass" else " open"
        out.append(f"<details id=\"{e(r['scene'])}\"{open_}><summary>{e(r['scene'])}"
                   f"<span class=\"badge {r['status']}\">{r['status']}</span></summary>")
        if r.get("description"):
            out.append(f"<p class=desc>{e(r['description'])}</p>")
        for n in r["notes"]:
            out.append(f"<p class=desc>{e(n)}</p>")
        out.append("<div class=frames>")
        for f in r["frames"]:
            def mv(k, label, digits, scale=1.0):
                v = f[k] * scale
                cls = "bad" if k in f["fails"] else ""
                return f"{label} <b class=\"{cls}\">{v:.{digits}f}</b>"
            diff_line = " &middot; ".join([mv("mae", "MAE", 3), mv("ssim", "SSIM", 4), mv("tile_mae", "tile", 1),
                                           mv("bad", "bad %", 2, 100), f"PSNR {f['psnr']:.1f} dB"])
            range_line = " &middot; ".join([mv("env_bad", "outside %", 3, 100), mv("env_tile", "worst tile", 2),
                                            mv("env_mae", "mean", 3)])
            if f.get("note"):
                range_line += f" &middot; <b class=bad>{e(f['note'])}</b>"
            imgs = f.get("img", {})
            status = "<b class=bad>fails: " + e(", ".join(f["fails"])) + "</b>" if f["fails"] else "passes"
            out.append("<div class=frame>")
            out.append(f"<figure><a href=\"{e(Path(f['golden']).as_uri())}\"><img loading=lazy src=\"{imgs.get('golden', '')}\" alt=golden></a>"
                       f"<figcaption>golden, closest frame ({f['golden_time']} s)</figcaption></figure>")
            out.append(f"<figure><a href=\"{e(Path(f['test']).as_uri())}\"><img loading=lazy src=\"{imgs.get('test', '')}\" alt=test></a>"
                       f"<figcaption>test, {f['time']} s: {status}</figcaption></figure>")
            out.append(f"<figure><img loading=lazy src=\"{imgs.get('diff', '')}\" alt=\"difference heat map\">"
                       f"<figcaption>difference from the closest golden frame</figcaption><p class=m>{diff_line}</p></figure>")
            out.append(f"<figure><img loading=lazy src=\"{imgs.get('range', '')}\" alt=\"outside the golden range\">"
                       f"<figcaption>outside the golden range</figcaption><p class=m>{range_line}</p></figure>")
            out.append("</div>")
        out.append("</div>")
        d = r.get("drawlog")
        if d:
            cls = "bad" if d.get("fail") else ""
            out.append(f"<p class=m>Draw list similarity <b class=\"{cls}\">{d['ratio']:.4f}</b> (needs "
                       f"{th.get('drawlog', 0)}); draws {d['golden_draws']} golden / {d['test_draws']} test, resolves "
                       f"{d['golden_resolves']} / {d['test_resolves']}. "
                       f"<a href=\"{e(d.get('diff_file', ''))}\">diff</a></p>")
            if d["only_golden"] or d["only_test"]:
                out.append("<p class=m>Shader pairs drawn more often in golden:</p><ul>"
                           + "".join(f"<li><code>{e(x)}</code></li>" for x in d["only_golden"]) + "</ul>")
                out.append("<p class=m>Shader pairs drawn more often in test:</p><ul>"
                           + "".join(f"<li><code>{e(x)}</code></li>" for x in d["only_test"]) + "</ul>")
        out.append("</details>")
    out.append("</main></body></html>")
    return "\n".join(out)


# ---------------------------------------------------------------------------


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--scenes-file", default=str(DEFAULT_SCENES))
    sub = p.add_subparsers(dest="cmd", required=True)

    q = sub.add_parser("plan", help="print the runs to launch for the chosen scenes (JSON)")
    q.add_argument("--scenes", default="all")
    q.add_argument("--plugin", default="")
    q.add_argument("--golden-root", default="")
    q.add_argument("--golden", action="store_true", help="plan a golden capture (adds the golden-only times)")
    q.add_argument("--no-frame-log", action="store_true")
    q.set_defaults(fn=cmd_plan)

    q = sub.add_parser("collect", help="copy a finished run's shots and frame log into scene folders")
    q.add_argument("--run", required=True)
    q.add_argument("--run-dir", required=True)
    q.add_argument("--out-root", required=True)
    q.add_argument("--scenes", default="all")
    q.add_argument("--plugin", default="")
    q.add_argument("--label", default="")
    q.add_argument("--trigger", default="", help="the REX_DEV_FRAME_LOG value the run used")
    q.add_argument("--golden", action="store_true", help="also copy the golden-only times")
    q.set_defaults(fn=cmd_collect)

    q = sub.add_parser("compare", help="compare a capture with the golden set and write report.html")
    q.add_argument("--golden-root", required=True)
    q.add_argument("--test-root", required=True)
    q.add_argument("--scenes", default="all")
    q.add_argument("--report-dir", default="")
    q.add_argument("--plugin", default="")
    q.set_defaults(fn=cmd_compare)

    q = sub.add_parser("calibrate", help="suggest thresholds from two captures of the same plugin")
    q.add_argument("--golden-root", required=True)
    q.add_argument("--test-root", required=True)
    q.add_argument("--scenes", default="all")
    q.add_argument("--margin", type=float, default=2.0)
    q.add_argument("--floor-mae", type=float, default=0.5)
    q.add_argument("--floor-ssim", type=float, default=0.002)
    q.add_argument("--floor-tile", type=float, default=3.0)
    q.add_argument("--floor-bad", type=float, default=0.001)
    q.add_argument("--floor-drawlog", type=float, default=0.02)
    q.add_argument("--floor-env-tile", type=float, default=0.5)
    q.set_defaults(fn=cmd_calibrate)

    q = sub.add_parser("framelog", help="print a frame log canonically, or diff two")
    q.add_argument("log", help="a game log or a scene's framelog.txt")
    q.add_argument("other", nargs="?", help="a second log to diff against the first")
    q.add_argument("--strict", action="store_true", help="also compare EDRAM bases, offsets and addresses")
    q.set_defaults(fn=cmd_framelog)

    a = p.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
