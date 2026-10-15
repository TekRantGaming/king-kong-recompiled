# Task 100: F8 graphics panel leftovers

Completed on branch `f8-beta6`, based on released Beta 5 `e890ece`.
Read `F:\KK-native-renderer\DECISIONS.md` before the task and followed `GAME-RULES.md`.
No conflict with the standing decisions required skipping any part of the task.

## Changes

- Kept F8/F7/F6. Added the task's suggested simultaneous left-stick-click + right-stick-click chord to open/close the full panel. A held chord toggles once; either single click still belongs to the game. From compact mode the chord expands the panel, matching F8.
- D-pad or left stick up/down moves between visible settings and the footer controls. Left/right changes choices and sliders. A activates toggles or footer buttons; B closes. LB compares with saved settings while held; RB captures. The launcher input helper supplies the same stick thresholds and direction-repeat timings. Focus is outlined, follows the setting when conditional rows change, scrolls into view and shows the translated help.
- Captured pad samples enter before controller remapping and toggle-aim processing. All buttons, triggers and both sticks are zeroed before returning to the game while the full panel is open. The opening sample is consumed too. Closing keeps controls hidden until neutral, preventing held close/compare/fire/movement inputs from reaching play. Toggle aim is cleared when capture consumes a sample. Pad input crosses to the UI under a mutex; stale input releases comparison. Keyboard F7 and controller LB can overlap without one release ending the other hold.
- Used the existing `user_language`, `text::T` and `text::F` lookup. Added `debug_overlay_text_table.inc` inside `launcher_text_table.inc`. German, Spanish, French and Italian cover panel headings, setting labels/options/help, live readouts, preparation notices, restart explanations, buttons, capture messages and settings-export labels. Technical names and units remain unchanged. Longer hints and restart descriptions wrap; Save and Reset each have their own footer row.
- FSR 4 is selectable only with active DirectX 12, AMD vendor `0x1002`, a completed capability query and a positive FSR 4 result. An unavailable saved upscaler is only a preview and is not inserted into the dropdown. Both mouse and controller use the filtered choices. Shared choice values and selection logic are exercised with mocked capabilities, including setting the actual `native_upscaler` cvar to `fsr4` when eligible.

## Build and automated checks

Built `kk-dev-vulkan` successfully with the permanent SDK links already supplied by `codex\mkwt.ps1`. Vulkan and DirectX 12 and all three upscaler SDKs were enabled. Explicitly configured:

```text
-DPython3_EXECUTABLE=C:/Users/TekRa/AppData/Local/Programs/Python/Python312/python.exe
-DNR_DXC_RUNTIME=F:/KK-native-renderer/downloads/dxc-v1.9.2609/dxc/bin/x64/dxcompiler.dll
```

`ctest --test-dir kk/out/build/kk-dev-vulkan --output-on-failure`: **5/5 passed**:
hardware-recommendation, launcher-logic, graphics-panel, graphics-panel-text and startup-recovery.
The new console harness passed **591 checks**, covering input capture/release, single-click preservation, chord edges, unsupported choices, the API/vendor/capability matrix and actual translation lookups in every language.
The text audit covers **135 panel text keys** in all five languages and validates placeholders and the no-em-dash rule.
`git diff --check` passed.

The extended in-game harness is `KK_DEV_PANEL_CONTROLLER=1` in `debug_overlay.cpp`; `codex/test-panel-100.ps1` runs it with the required private data, game lock and background-window protections. Its runner refuses a third task-100 launch. Existing full panel stress tests remain opt-in; they were not run as part of these two short controller tests.

## Game tests: exactly two launches

Both runs used chapter 27, private `userdata`, a copied executable/DLLs/glyphs/shader packs, 60 FPS, `--fullscreen=false --window_width=1280 --window_height=720 --kk_fullscreen_mode=desktop`, both SDL activation hints set to 0 and the already-running kkbg2 helper. The renderer logs confirm the requested API, with no startup fallback. Only the launched parent and its own child PID were stopped. Each run held and released the shared game lock. No user-owned game, Documents saves/cache or Desktop game copy was touched. Nothing was deleted.

1. `F:\KK-native-renderer\runs\task100-panel-vulkan-20261015-111510` (about 18 seconds). **17 passed, 1 harness failure**: the capture check ran 200 ms after requesting the PNG, before asynchronous encoding finished. The log records the completed PNG about 600 ms after capture; the file subsequently decoded successfully. Opening, held-chord behavior, D-pad and stick navigation, changing FXAA, LB/F7 comparison, input blocking, closing by chord/B and neutral release all passed. Fixed the harness to allow up to five seconds for background encoding.
2. `F:\KK-native-renderer\runs\task100-panel-d3d12-20261015-111818` (about 24 seconds). **18 passed, 0 failed**, including completed PNG capture. The final harness also cycled all five languages. Reviewed `panel-language-1.png`, `panel-language-3.png`, `panel-language-5.png`, `panel-language-4.png` and `panel-language-6.png`: labels/help, accented glyphs, focus and footer buttons were readable and fit. The transient capture notice retains the language in which it was generated. All own-window and game-frame PNGs from both runs decoded successfully.

Own-window images used PrintWindow with per-thread DPI awareness; captures never included the desktop or activated a window. Both game-frame captures are 3840x2160 PNGs with translated settings sidecars. Logs, frames, shader packs and other game-derived files remain only in the private run folders and are not committed. Each run logged the two startup `ResolvePath() failed - device not found` messages also documented in REPORT-076; this task did not change that filesystem behavior.

## Limits and handoff

**FSR 4 is untested on real AMD hardware.** This PC has an NVIDIA RTX 4070 Ti. The capability/selection checks use mocks; the two renderer runs do not validate FSR 4 image output. Controller input was scripted through the real game pad-read hook; no physical controller was tested. The long stress harness and every possible setting/footer operation were not replayed in the game. The final controller checks were run on DirectX 12; Vulkan's earlier capture timing failure is retained above rather than reported as a clean harness pass.

Only task-100 source, translation, harness and report files are included. Restored build-generated line-ending-only changes to the manifest and generated CMake file. No game-derived data, prepared package, push, merge, beta-line update or release. No further game launches are allowed for this task.
