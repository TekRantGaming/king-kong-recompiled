
## Task 206 (pause-b9): brightness/gamma, Performance metrics, pause menu greyed rows
- kk_brightness (-50..50, black level) / kk_gamma (0.5..1.5, mids): Display > Picture; renderer applies them at
  Present via the gamma ramp (native-renderer/backend/picture_adjust.h, game_passes.cpp); identity = old path.
  Pause backdrop is retouched (RetouchTable) while paused. Tests: picture-and-metrics (kk/tests/perf_overlay_tests.cpp).
- kk_performance_metrics off|fps|full (Display > Overlay, F2 cycles; kk_show_fps=true migrates to fps,
  MigrateFrameCounter). overlay.cpp: MetricsSampler worker (benchmark::SystemSampler, nr_benchmark_stats) only
  while Full; hidden while the pause menu is open and during a benchmark. Cost: draw ~15-22 us, sampler ~0.8 ms
  per 0.5 s on its worker; FPS A/B off 145.7 vs full 144.0 (noise +-3%).
- Pause menu: LiveOnly greys restart-only rows (kInLauncherOnly note); frame rate and temporal upscaler live
  (in game the offered list comes from the running device: OfferedWhilePlaying, never QueryUpscalers, which in the
  game process answered "DLSS no" from a D3D12 probe). Pause list has a Graphics entry.
- Tools: wt\pause-b9\kk\out\task206\ run206.ps1 (chapter run: -Api, -Shots, -PauseActions, -PauseInput, -ExtraEnv;
  use KK_DEV_SCRIPT=300:BACK;KK_DEV_SHOTS_FROM=menu or autoskip's Start presses resume the pause), runL.ps1 +
  stepsL.txt (launcher), diff.py, prepost.py. KK_DEV_SHOTS_LOG=1 logs "KK dev: shot win_<name>" for PrintWindow.
