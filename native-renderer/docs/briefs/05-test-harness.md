# Brief 05: the test harness

## Goal

An unattended way to say "the new renderer draws scene X the same as today's renderer": scripted runs that
capture golden frames and draw logs from the Xenos plugin for a fixed set of scenes, the same runs with
`rexgpu-native`, and a comparison report (per-scene difference score, heat maps, pass / fail thresholds).
Output: `native-renderer/tests/` (scripts and the compare tool) and `docs/testing.md`.

## Inputs

- The port's dev aids (`kk/src/dev_tools.cpp`): `KK_DEV_AUTOSKIP`, `KK_DEV_SCRIPT` (timed button presses),
  `KK_DEV_SHOTS` / `KK_DEV_SHOTS_FROM` / `KK_DEV_SHOTS_DIR` (frame captures of the guest output as BMP),
  `KK_DEV_WANDER`; the scripts used in this session (`analysis/` run commands: the V-Rex script with the
  test save, the chapter-select script generator `tourgen.py` in the main repo's scratch notes).
- `REX_DEV_FRAME_LOG=<seconds>[,<frames>]` in the SDK's D3D12 and Vulkan backends: every draw and resolve
  of a frame (shaders, targets, depth state, counts). The native plugin should emit the same log format so
  draw lists can be diffed line by line.
- The test save (`F:\KK-native-renderer\userdata`, profile `B13EBABEBABEBABE`) and shader cache
  (`F:\KK-native-renderer\cache`).

## Method

1. Scene set: title screen, save menu, a pre-rendered video, the Venture opening (scripted), V-Rex at three
   script times, a Kong chapter, the pause menu, a loading screen. Each scene = chapter + script + capture
   times. Scenes must be reproducible: fixed frame-rate cap (30), no wander, same resolution (1280x720).
2. Golden run: capture frames (BMP) and the frame log for each scene from the Xenos plugin; store under
   `F:\KK-native-renderer\golden\<scene>\` (never in git).
3. Compare tool (Python, PIL / numpy): per-frame difference (mean absolute error, SSIM-like score, a heat map
   PNG), draw-log diff, a summary table, thresholds per scene (animated fire and rain need tolerance).
4. A one-command runner: `tests/run.ps1 -Plugin native -Scenes all` runs everything unattended (checks no
   other `king_kong` is running first), writes `report.html`.
5. Later: Linux runs on the Ally through SSH (the main project's `ally_*.sh` scripts show the pattern).

## Done when

The golden set exists for all scenes, running the comparison against the Xenos plugin itself reports a pass
(self-consistency), and the report is readable at a glance.
