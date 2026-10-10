#!/usr/bin/env python3
"""Tests for compare.py with synthetic images and logs (no game data needed).

Run from anywhere:  python -I native-renderer/tests/test_compare.py   (or python -m unittest in this folder)
"""

from __future__ import annotations

import contextlib
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare as C  # noqa: E402

W, H = 160, 90  # small frames keep the tests fast; tiles are 10x10 here


# ---------------------------------------------------------------------------
# Synthetic data


def write_game_bmp(path: Path, rgb: np.ndarray):
    """A 32-bit top-down BMP exactly as kk::art::SaveTitleCapture writes it (BGRA, alpha 255)."""
    h, w = rgb.shape[:2]
    px = np.empty((h, w, 4), np.uint8)
    px[..., 0], px[..., 1], px[..., 2], px[..., 3] = rgb[..., 2], rgb[..., 1], rgb[..., 0], 255
    hdr = b"BM" + struct.pack("<IIIIiiHHIIiiII", 54 + px.nbytes, 0, 54, 40, w, -h, 1, 32, 0, px.nbytes,
                              2835, 2835, 0, 0)
    path.write_bytes(hdr + px.tobytes())


def write_bmp24_bottom_up(path: Path, rgb: np.ndarray):
    h, w = rgb.shape[:2]
    stride = (w * 3 + 3) // 4 * 4
    rows = np.zeros((h, stride), np.uint8)
    rows[:, : w * 3] = rgb[::-1, :, ::-1].reshape(h, w * 3)
    rows[:, w * 3:] = 0xAB  # padding must be ignored
    hdr = b"BM" + struct.pack("<IIIIiiHHIIiiII", 54 + rows.nbytes, 0, 54, 40, w, h, 1, 24, 0, rows.nbytes,
                              0, 0, 0, 0)
    path.write_bytes(hdr + rows.tobytes())


def write_bmp32_bitfields_rgba(path: Path, rgb: np.ndarray):
    """BI_BITFIELDS with R in the lowest byte (masks after a 40-byte header)."""
    h, w = rgb.shape[:2]
    px = np.empty((h, w, 4), np.uint8)
    px[..., :3], px[..., 3] = rgb, 0
    masks = struct.pack("<III", 0xFF, 0xFF00, 0xFF0000)
    off = 54 + len(masks)
    hdr = b"BM" + struct.pack("<IIIIiiHHIIiiII", off + px.nbytes, 0, off, 40, w, -h, 1, 32, 3, px.nbytes,
                              0, 0, 0, 0)
    path.write_bytes(hdr + masks + px.tobytes())


def background(seed: int = 0) -> np.ndarray:
    """A still picture: gradient plus a 'text' block, the kind of thing that must match exactly."""
    y, x = np.mgrid[0:H, 0:W]
    img = np.stack([x * 255 // W, y * 255 // H, (x + y) * 255 // (W + H)], axis=2).astype(np.uint8)
    img[10:20, 10:50] = (240, 240, 240)  # menu text / HUD element, far from the animated part
    return img


def with_streak(img: np.ndarray, x: int, width: int = 2, colour=(200, 210, 255)) -> np.ndarray:
    """A rain streak (thin vertical line) in the animated area, rows 40-80."""
    out = img.copy()
    out[40:80, x:x + width] = colour
    return out


def golden_burst():
    """Golden frames of a 'rain' scene: one streak at x = 100, 110, ... 140 over the still picture."""
    bg = background()
    return [with_streak(bg, x) for x in range(100, 141, 10)]


# ---------------------------------------------------------------------------


class ImageLoading(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        rng = np.random.default_rng(1)
        self.img = rng.integers(0, 256, (13, 7, 3), dtype=np.uint8)  # odd width: row padding in 24-bit BMPs

    def tearDown(self):
        self.tmp.cleanup()

    def test_game_bmp(self):
        p = self.dir / "shot_80.bmp"
        write_game_bmp(p, self.img)
        np.testing.assert_array_equal(C.load_image(p), self.img)

    def test_bmp24_bottom_up_with_padding(self):
        p = self.dir / "a.bmp"
        write_bmp24_bottom_up(p, self.img)
        np.testing.assert_array_equal(C.load_image(p), self.img)

    def test_bmp_bitfields(self):
        p = self.dir / "a.bmp"
        write_bmp32_bitfields_rgba(p, self.img)
        np.testing.assert_array_equal(C.load_image(p), self.img)

    def test_bmp_written_by_pillow(self):
        p = self.dir / "a.bmp"
        Image.fromarray(self.img).save(p)
        np.testing.assert_array_equal(C.load_image(p), self.img)

    def test_png_rgb_rgba_palette_grey16(self):
        p = self.dir / "rgb.png"
        Image.fromarray(self.img).save(p)
        np.testing.assert_array_equal(C.load_image(p), self.img)

        rgba = np.dstack([self.img, np.full(self.img.shape[:2], 7, np.uint8)])
        p = self.dir / "rgba.png"
        Image.fromarray(rgba, "RGBA").save(p)
        np.testing.assert_array_equal(C.load_image(p), self.img)  # alpha dropped, colour kept

        pal = Image.fromarray(self.img).quantize(16)
        p = self.dir / "pal.png"
        pal.save(p)
        np.testing.assert_array_equal(C.load_image(p), np.asarray(pal.convert("RGB")))

        grey = (np.arange(7 * 13, dtype=np.uint16).reshape(13, 7) * 700)
        p = self.dir / "g16.png"
        Image.fromarray(grey).save(p)
        got = C.load_image(p)
        self.assertEqual(got.shape, (13, 7, 3))
        np.testing.assert_array_equal(got[..., 0], (grey >> 8).astype(np.uint8))

    def test_bmp_and_png_load_the_same(self):
        write_game_bmp(self.dir / "a.bmp", self.img)
        Image.fromarray(self.img).save(self.dir / "a.png")
        np.testing.assert_array_equal(C.load_image(self.dir / "a.bmp"), C.load_image(self.dir / "a.png"))

    def test_cut_short_and_garbage(self):
        p = self.dir / "cut.bmp"
        write_game_bmp(p, self.img)
        p.write_bytes(p.read_bytes()[:-40])
        with self.assertRaises(C.ImageError):
            C.load_image(p)
        p = self.dir / "junk.png"
        p.write_bytes(b"not an image")
        with self.assertRaises(C.ImageError):
            C.load_image(p)

    def test_find_shot_prefers_bmp_then_png(self):
        self.assertIsNone(C.find_shot(self.dir, 8.0))
        Image.fromarray(self.img).save(self.dir / "shot_80.png")
        self.assertEqual(C.find_shot(self.dir, 8.0).name, "shot_80.png")
        write_game_bmp(self.dir / "shot_80.bmp", self.img)
        self.assertEqual(C.find_shot(self.dir, 8.0).name, "shot_80.bmp")


class FrameScores(unittest.TestCase):
    def test_identical(self):
        bg = background()
        m = C.metrics(bg, bg, 40)
        self.assertEqual(m["mae"], 0)
        self.assertEqual(m["ssim"], 1.0)
        self.assertEqual(m["psnr"], 99.0)
        self.assertEqual(m["tile_mae"], 0)
        self.assertEqual(m["bad"], 0)

    def test_small_missing_object_shows_in_tile_mae(self):
        bg = background()
        t = bg.copy()
        t[10:20, 10:20] = 0  # a quarter of the 'text' block gone: 1% of the frame
        m = C.metrics(bg, t, 40)
        self.assertLess(m["mae"], 3)  # the whole-frame mean hardly moves...
        self.assertGreater(m["tile_mae"], 200)  # ...but the tile it sits in is all wrong
        self.assertAlmostEqual(m["bad"], 100 / (W * H), places=5)
        self.assertLess(m["ssim"], 0.995)

    def test_uniform_small_offset(self):
        bg = background()
        t = np.clip(bg.astype(int) + 2, 0, 255).astype(np.uint8)
        m = C.metrics(bg, t, 40)
        self.assertLessEqual(m["mae"], 2)
        self.assertEqual(m["bad"], 0)
        self.assertGreater(m["ssim"], 0.99)

    def test_ignore_mask(self):
        bg = background()
        t = bg.copy()
        t[0:30, 0:60] = 0
        mask = np.zeros((H, W), bool)
        mask[0:30, 0:60] = True
        self.assertEqual(C.metrics(bg, t, 40, mask)["mae"], 0)

    def test_heat_map_thumbnail_keeps_single_pixel(self):
        g = np.zeros((720, 1280, 3), np.uint8)
        d = np.zeros((720, 1280), np.uint8)
        d[333, 777] = 255
        im = np.asarray(C.heat_image(g, d, width=640))
        self.assertEqual(im.shape, (360, 640, 3))
        self.assertEqual(int(im.max()), 255)  # a box filter would have made it 1/4 as bright
        full = np.asarray(C.heat_image(g, d))
        self.assertEqual(full.shape, (720, 1280, 3))
        self.assertEqual(int(full[0, 0].max()), 0)  # no difference = black


class ToleranceModel(unittest.TestCase):
    """The golden range: still parts must match, animated parts may do what the golden frames did."""

    def outside(self, rng, t, tol=16):
        return C.envelope_metrics(rng, t, tol)

    def test_still_scene_has_no_range(self):
        bg = background()
        rng = C.GoldenRange([bg, bg, bg])
        self.assertEqual(rng.animated_share, 0)
        self.assertEqual(self.outside(rng, bg)["env_bad"], 0)
        t = bg.copy()
        t[10:20, 10:50] = 0  # the text block missing
        r = self.outside(rng, t)
        self.assertGreater(r["env_bad"], 0.02)
        self.assertGreater(r["env_tile"], 100)

    def test_animated_part_accepts_what_golden_showed(self):
        rng = C.GoldenRange(golden_burst())
        self.assertGreater(rng.animated_share, 0)
        self.assertLess(rng.animated_share, 0.05)
        # a golden position, but on a test frame
        self.assertEqual(self.outside(rng, with_streak(background(), 120))["env_bad"], 0)
        # no streak at all (between drops) is also something every golden pixel showed
        self.assertEqual(self.outside(rng, background())["env_bad"], 0)

    def test_streak_between_golden_positions_needs_radius(self):
        t = with_streak(background(), 104)
        strict = C.GoldenRange(golden_burst(), radius=0)
        self.assertGreater(self.outside(strict, t)["env_bad"], 0)
        loose = C.GoldenRange(golden_burst(), radius=4)
        self.assertEqual(self.outside(loose, t)["env_bad"], 0)

    def test_radius_does_not_loosen_still_parts(self):
        rng = C.GoldenRange(golden_burst(), radius=4)
        t = with_streak(background(), 104)
        t[10:20, 10:50] = 0  # still text missing while the rain is fine
        r = self.outside(rng, t)
        self.assertAlmostEqual(r["env_bad"], 400 / (W * H), places=5)
        # the edge of the text block, one pixel off, must not hide behind the radius either
        t = background()
        t[10:20, 50] = (240, 240, 240)
        self.assertGreater(self.outside(rng, t)["env_bad"], 0)

    def test_wrong_colour_in_animated_part_fails(self):
        rng = C.GoldenRange(golden_burst(), radius=4)
        t = with_streak(background(), 120, colour=(0, 255, 0))
        self.assertGreater(self.outside(rng, t)["env_bad"], 0)

    def test_object_outside_animated_area_fails(self):
        rng = C.GoldenRange(golden_burst(), radius=4)
        t = with_streak(background(), 30)  # rain where it never was (in the still part)
        self.assertGreater(self.outside(rng, t)["env_bad"], 0)

    def test_small_noise_within_tolerance(self):
        rng = C.GoldenRange(golden_burst())
        noise = np.random.default_rng(3).integers(-10, 11, (H, W, 3))
        t = np.clip(background().astype(int) + noise, 0, 255).astype(np.uint8)
        r = self.outside(rng, t, tol=16)
        self.assertEqual(r["env_bad"], 0)
        self.assertGreater(r["env_mae"], 0)
        self.assertGreater(self.outside(rng, t, tol=4)["env_bad"], 0)

    def test_anim_level_ignores_dither_noise(self):
        bg = background()
        rng0 = np.random.default_rng(5)
        frames = [np.clip(bg.astype(int) + rng0.integers(-2, 3, bg.shape), 0, 255).astype(np.uint8)
                  for _ in range(4)]
        self.assertGreater(C.GoldenRange(frames, radius=3).animated_share, 0.5)
        tight = C.GoldenRange(frames, radius=3, anim_level=4)
        self.assertEqual(tight.animated_share, 0)
        t = bg.copy()
        t[10:20, 50:53] = (240, 240, 240)  # text block 3 px too wide: radius must not apply (nothing animated)
        self.assertGreater(self.outside(tight, t)["env_bad"], 0)

    def test_ignore_regions_scale_to_frame(self):
        # ignore rectangles are given for 1280x720; on a 160x90 frame they shrink by 8
        rng = C.GoldenRange([background()], ignore=[[0, 0, 640, 240]])
        self.assertEqual(int(rng.ignore.sum()), 80 * 30)
        t = background()
        t[0:30, 0:80] = 0
        self.assertEqual(self.outside(rng, t)["env_bad"], 0)
        t[50, 120] = 0
        self.assertGreater(self.outside(rng, t)["env_bad"], 0)


# ---------------------------------------------------------------------------
# Frame logs

D3D_DRAW = ("draw {i} ps {ps:016X} vs 00000000000000AB pitch 1280 msaa 1 z test+write @{db}:f0 off 0,0 "
            "scissor 1280x720 rt0@{cb}:f6 count {n}")
VK_DRAW = ("draw {i} ps {ps:016X} vs 00000000000000AB pitch 1280 msaa 1 z test+write func 3 @{db}:f0 off 0,0 "
           "scissor 1280x720 rt0@{cb}:f6 count {n}")
RESOLVE = ("resolve {i} rt0@0:f6 -> 1F000000 format 6 pitch 1280 height 720 command 0 clear color 1 depth 1 "
           "off 0,0 surface pitch 1280 msaa 1")


def frame_lines(draws, fmt=D3D_DRAW, base=0, extra=()):
    lines = [fmt.format(i=i, ps=ps, db=base + 320, cb=base, n=n) for i, (ps, n) in enumerate(draws)]
    lines += list(extra)
    lines.append(RESOLVE.format(i=0))
    lines.append(f"frame end, {len(draws)} draws, 1 resolves")
    return lines


def game_log(frames, style="rex"):
    """A game log with the frame log in it: the SDK's layout, or some other layout (the fallback parser)."""
    out = []
    sec = 10

    def put(msg, level="warning"):
        nonlocal sec
        sec += 1
        if style == "rex":
            out.append(f"[2026-10-10 04:00:{sec:02d}.000] [{level}] [gpu] [t7] {msg}")
        else:
            out.append(f"2026-10-10T04:00:{sec:02d}.5 gpu {level.upper()}: {msg}")

    put("KK: save menu opened", "info")
    put("Frame log: frame start")
    for lines in frames:
        for line in lines:
            put("Frame log: " + line)
    put("Frame log: draw 9 ps 0000000000000001 vs 0000000000000002", "warning")  # cut off: no frame end
    return "\n".join(out) + "\n"


SCENE_DRAWS = [(0x11, 6), (0x22, 600), (0x22, 900), (0x33, 6)]


class FrameLogs(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name, text):
        p = self.dir / name
        p.write_text(text, encoding="utf-8")
        return p

    def test_blocks_from_game_log(self):
        for style in ("rex", "other"):
            p = self.write(f"{style}.log", game_log([frame_lines(SCENE_DRAWS), frame_lines(SCENE_DRAWS[:3])], style))
            frames = C.read_framelog(p)
            self.assertEqual(len(frames), 2, style)  # the cut-off third frame is dropped
            self.assertEqual(frames[0][-1], "frame end, 4 draws, 1 resolves")
            st = C.frame_stats(frames[1])
            self.assertEqual((st["draws"], st["resolves"]), (3, 1))
            self.assertEqual(st["problems"], [])

    def test_canonical_keys_loose_and_strict(self):
        a = frame_lines(SCENE_DRAWS, base=0)
        b = frame_lines(SCENE_DRAWS, VK_DRAW, base=1000)  # other EDRAM bases, Vulkan's func field
        self.assertEqual(C.frame_keys(a), C.frame_keys(b))
        self.assertNotEqual(C.frame_keys(a, strict=True), C.frame_keys(b, strict=True))
        k = C.frame_keys(a)[0]
        self.assertIn("ps=0000000000000011", k)
        self.assertIn("rt=0:f6", k)
        self.assertIn("ds=f0", k)

    def test_compare_identical_and_changed(self):
        g = [frame_lines(SCENE_DRAWS), frame_lines(SCENE_DRAWS[:3])]
        r = C.compare_framelogs(g, [frame_lines(SCENE_DRAWS)])
        self.assertEqual(r["ratio"], 1.0)
        self.assertEqual(r["golden_frame"], 0)
        self.assertEqual(r["diff"], [])

        missing = [d for d in SCENE_DRAWS if d != (0x33, 6)] + [(0x44, 6)]
        r = C.compare_framelogs([frame_lines(SCENE_DRAWS)], [frame_lines(missing)])
        self.assertLess(r["ratio"], 1.0)
        self.assertTrue(any(x.startswith("1x draw ps=0000000000000033") for x in r["only_golden"]))
        self.assertTrue(any(x.startswith("1x draw ps=0000000000000044") for x in r["only_test"]))
        self.assertTrue(any(line.startswith("-draw ps=0000000000000033") for line in r["diff"]))
        self.assertTrue(any(line.startswith("+draw ps=0000000000000044") for line in r["diff"]))

    def test_log_problems_are_reported(self):
        bad = frame_lines(SCENE_DRAWS, VK_DRAW, extra=["draw 3 pipeline placeholder (skipped)", "something odd"])
        bad[-1] = "frame end, 9 draws, 1 resolves"
        st = C.frame_stats(bad)
        self.assertEqual(st["skipped"], 1)
        self.assertEqual(len(st["problems"]), 3)
        r = C.compare_framelogs([frame_lines(SCENE_DRAWS)], [bad])
        self.assertEqual(r["ratio"], 1.0)  # the draw list itself is the same
        self.assertEqual(len(r["problems"]), 3)

    def test_framelog_command(self):
        g = self.write("g.log", game_log([frame_lines(SCENE_DRAWS)]))
        t = self.write("t.log", game_log([frame_lines(SCENE_DRAWS[:2])]))
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(C.main(["framelog", str(g)]), 0)
        self.assertIn("# frame 0: 4 draws, 1 resolves", out.getvalue())
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(C.main(["framelog", str(g), str(t), "--min-ratio", "0.99"]), 1)
        self.assertIn("similarity", out.getvalue())
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(C.main(["framelog", str(g), str(g), "--min-ratio", "0.99"]), 0)
            self.assertEqual(C.main(["framelog", str(self.write("empty.log", "nothing\n"))]), 2)


# ---------------------------------------------------------------------------
# The whole compare: scene folders, thresholds, report


def scenes_file(path: Path, thresholds=None, ignore=None) -> Path:
    th = {"mae": None, "ssim": None, "tile_mae": None, "bad": None, "bad_level": 40, "env_tol": 16,
          "env_radius": 4, "env_anim_level": 0, "env_bad": 0.002, "env_tile": 3.0, "drawlog": 0.98, "min_pass": 0.6}
    th.update(thresholds or {})
    rain = {"name": "rain", "run": "r", "description": "synthetic rain", "times": [8.0, 9.0, 10.0],
            "golden_times": {"from": 7.0, "to": 11.0, "step": 1.0}}
    if ignore:
        rain["ignore"] = ignore
    data = {
        "defaults": {"args": [], "env": {}, "clock_offset": {"menu": 7.7}, "thresholds": th},
        "runs": {"r": {"from": "menu", "script": "12:A", "frame_log": {"scene": "rain", "at": 9.0, "frames": 2}}},
        "scenes": [rain,
                   {"name": "still", "run": "r", "times": [20.0], "thresholds": {"mae": 1.0, "ssim": 0.99,
                                                                                  "min_pass": 1.0}}],
    }
    path.write_text(json.dumps(data), encoding="utf-8")
    return path


class EndToEnd(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.golden, self.test, self.report = root / "golden", root / "test", root / "report"
        self.scenes = scenes_file(root / "scenes.json")
        gr, gs = self.golden / "rain", self.golden / "still"
        gr.mkdir(parents=True)
        gs.mkdir(parents=True)
        bg = background()
        # golden burst 7..11 s: one streak per frame at x = 100, 110, ... 140 (game BMPs)
        for t, x in zip(range(70, 111, 10), range(100, 141, 10)):
            write_game_bmp(gr / f"shot_{t}.bmp", with_streak(bg, x))
        write_game_bmp(gs / "shot_200.bmp", bg)
        (gr / "framelog.txt").write_text(
            "frame 0\n" + "\n".join(frame_lines(SCENE_DRAWS)) + "\nframe 1\n"
            + "\n".join(frame_lines(SCENE_DRAWS)) + "\n", encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def put_test(self, streaks, still=None, framelog=SCENE_DRAWS):
        tr, ts = self.test / "rain", self.test / "still"
        tr.mkdir(parents=True, exist_ok=True)
        ts.mkdir(parents=True, exist_ok=True)
        for t, img in zip((80, 90, 100), streaks):
            Image.fromarray(img).save(tr / f"shot_{t}.png")  # PNG on the test side
        Image.fromarray(background() if still is None else still).save(ts / "shot_200.png")
        if framelog is not None:
            (tr / "framelog.txt").write_text("frame 0\n" + "\n".join(frame_lines(framelog)) + "\n", encoding="utf-8")

    def run_compare(self, *extra):
        with contextlib.redirect_stdout(io.StringIO()) as out:
            code = C.main(["--scenes-file", str(self.scenes), "compare", "--golden-root", str(self.golden),
                           "--test-root", str(self.test), "--report-dir", str(self.report), "--plugin", "native",
                           *extra])
        summary = json.loads((self.report / "summary.json").read_text(encoding="utf-8"))
        return code, summary, out.getvalue()

    def test_self_consistency_passes(self):
        bg = background()
        self.put_test([with_streak(bg, 104), with_streak(bg, 117), with_streak(bg, 130)])
        code, summary, out = self.run_compare()
        self.assertEqual(code, 0, out)
        self.assertEqual(summary["status"], "pass")
        rain = summary["scenes"][0]
        self.assertEqual(rain["passed_frames"], 3)
        self.assertEqual(rain["golden_frames"], 5)
        self.assertEqual(rain["drawlog"]["ratio"], 1.0)
        self.assertEqual(rain["frames"][2]["golden_time"], 10.0)  # streak at 130 = the 10 s golden frame
        html = (self.report / "report.html").read_text(encoding="utf-8")
        self.assertIn(">PASS<", html)
        self.assertTrue((self.report / "img" / "rain" / "shot_80_diff.png").exists())
        self.assertFalse((self.report / "img" / "rain" / "shot_80_diff_full.png").exists())

    def test_one_random_flash_is_allowed_two_are_not(self):
        bg = background()
        flash = np.clip(bg.astype(int) + 90, 0, 255).astype(np.uint8)  # lightning: the whole frame brighter
        self.put_test([flash, with_streak(bg, 110), with_streak(bg, 120)])
        code, summary, _ = self.run_compare()
        self.assertEqual(code, 0)
        self.assertEqual(summary["scenes"][0]["passed_frames"], 2)  # min_pass 0.6 of 3 = 2
        self.put_test([flash, flash, with_streak(bg, 120)])
        code, summary, _ = self.run_compare()
        self.assertEqual(code, 1)
        self.assertEqual(summary["scenes"][0]["status"], "fail")

    def test_missing_still_object_fails_with_heat_maps(self):
        bg = background()
        broken = bg.copy()
        broken[10:20, 10:50] = 0
        self.put_test([with_streak(bg, 110)] * 3, still=broken)
        code, summary, _ = self.run_compare()
        self.assertEqual(code, 1)
        still = summary["scenes"][1]
        self.assertEqual(still["status"], "fail")
        self.assertIn("mae", still["frames"][0]["fails"])
        self.assertIn("env_bad", still["frames"][0]["fails"])
        full = self.report / still["frames"][0]["img"]["diff_full"]
        self.assertTrue(full.exists())
        heat = np.asarray(Image.open(full))
        self.assertGreater(int(heat[15, 30].sum()), 600)  # the missing block is bright
        self.assertLess(int(heat[60, 20].sum()), 200)  # untouched parts stay dark
        html = (self.report / "report.html").read_text(encoding="utf-8")
        self.assertIn(">FAIL<", html)
        self.assertIn("<details id=\"still\" open>", html)

    def test_draw_list_change_fails(self):
        bg = background()
        self.put_test([with_streak(bg, 110)] * 3, framelog=SCENE_DRAWS[:2])
        code, summary, _ = self.run_compare()
        self.assertEqual(code, 1)
        dl = summary["scenes"][0]["drawlog"]
        self.assertTrue(dl["fail"])
        self.assertLess(dl["ratio"], 0.98)
        self.assertTrue((self.report / dl["diff_file"]).exists())
        self.assertIn("Draw list diff", (self.report / "report.html").read_text(encoding="utf-8"))

    def test_size_mismatch_fails(self):
        bg = background()
        self.put_test([with_streak(bg, 110)] * 3)
        Image.fromarray(np.zeros((H // 2, W // 2, 3), np.uint8)).save(self.test / "still" / "shot_200.png")
        code, summary, _ = self.run_compare()
        self.assertEqual(code, 1)
        self.assertIn("size", summary["scenes"][1]["frames"][0]["fails"])

    def test_missing_test_frames(self):
        bg = background()
        self.put_test([with_streak(bg, 110)] * 3)
        (self.test / "still" / "shot_200.png").unlink()
        code, summary, _ = self.run_compare()
        self.assertEqual(code, 2)
        self.assertEqual(summary["scenes"][1]["status"], "missing")

    def test_images_command(self):
        bg = background()
        self.put_test([with_streak(bg, 104), with_streak(bg, 117), with_streak(bg, 130)])
        with contextlib.redirect_stdout(io.StringIO()) as out:
            code = C.main(["--scenes-file", str(self.scenes), "images", str(self.golden / "rain"),
                           str(self.test / "rain"), "--set", "env_radius=4", "--out", str(self.report)])
        self.assertEqual(code, 0, out.getvalue())
        self.assertTrue((self.report / "report.html").exists())
        with contextlib.redirect_stdout(io.StringIO()):
            code = C.main(["--scenes-file", str(self.scenes), "images", str(self.golden / "rain" / "shot_70.bmp"),
                           str(self.test / "rain" / "shot_80.png"), "--set", "env_radius=0"])
        self.assertEqual(code, 1)  # one golden frame, streak elsewhere: outside the (empty) range


class CollectAndPlan(unittest.TestCase):
    def test_collect_copies_shots_and_cuts_the_frame_log(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            scenes = scenes_file(root / "scenes.json")
            run_dir, out = root / "run", root / "out"
            run_dir.mkdir()
            for t in (80, 90, 100, 200):
                write_game_bmp(run_dir / f"shot_{t}.bmp", background())
            log = ["[2026-10-10 04:00:00.000] [info] [kk] [t1] KK: save menu opened"]
            # shot second 0 is at 04:00:01; each shot is saved 0.05 s after its time
            for t in (80, 90, 100, 200):
                s = 1 + t / 10 + 0.05
                log.append(f"[2026-10-10 04:00:{s:06.3f}] [info] [kk] [t1] KK: saved frame shot_{t}.bmp ({W}x{H})")
            # frame log taken at 04:00:10.5, asked for at 17.2 s on the log's clock
            log.append("[2026-10-10 04:00:10.500] [warning] [gpu] [t7] Frame log: frame start")
            for line in frame_lines(SCENE_DRAWS):
                log.append(f"[2026-10-10 04:00:10.510] [warning] [gpu] [t7] Frame log: {line}")
            (run_dir / "game.log").write_text("\n".join(sorted(log)) + "\n", encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()):
                code = C.main(["--scenes-file", str(scenes), "collect", "--run", "r", "--run-dir", str(run_dir),
                               "--out-root", str(out), "--trigger", "17.2,2"])
            self.assertEqual(code, 0)
            self.assertEqual(sorted(p.name for p in (out / "rain").glob("shot_*")),
                             ["shot_100.bmp", "shot_80.bmp", "shot_90.bmp"])
            meta = json.loads((out / "rain" / "meta.json").read_text(encoding="utf-8"))
            self.assertEqual(meta["frame_log"]["scene_time"], 9.45)
            self.assertEqual(meta["frame_log"]["clock_offset"], 7.75)
            frames = C.read_framelog(out / "rain" / "framelog.txt")
            self.assertEqual(len(frames), 1)
            self.assertEqual(C.frame_stats(frames[0])["draws"], 4)

    def test_plan(self):
        with tempfile.TemporaryDirectory() as tmp:
            scenes = scenes_file(Path(tmp) / "scenes.json")
            with contextlib.redirect_stdout(io.StringIO()) as out:
                self.assertEqual(C.main(["--scenes-file", str(scenes), "plan", "--scenes", "rain", "--golden"]), 0)
            run = json.loads(out.getvalue())["runs"][0]
            self.assertEqual(run["env"]["KK_DEV_SHOTS"], "7,8,9,10,11")
            self.assertEqual(run["expected"][0], "shot_70.bmp")
            self.assertEqual(run["env"]["REX_DEV_FRAME_LOG"], "16.70,2")

    def test_real_scene_list_loads(self):
        data = C.load_scenes(C.DEFAULT_SCENES)
        self.assertTrue(data["scenes"])
        for s in data["scenes"]:
            th = C.thresholds_for(data, s)
            self.assertIn("env_radius", th)


if __name__ == "__main__":
    unittest.main()
