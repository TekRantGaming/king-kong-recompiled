# Task 338: fixed-camera, interleaved effect/preset measurements using existing dev tools.
param([ValidateSet('matrix','focused','scale','benchmark','capture')][string]$Mode='matrix',
      [ValidateSet('vulkan','d3d12')][string]$Api='vulkan',
      [int]$Chapter=8, [string]$Scale='1.5', [int]$Repeat=1,
      [datetime]$NotBefore=[datetime]::MinValue,
      [ValidateSet('quality','balanced','performance')][string]$Quality='balanced',
      [ValidateSet('quality','balanced','performance')][string]$DeckAfter='performance')
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$runs='F:\KK-native-renderer\runs'
$job='codex-338'
$slot='F:\KK-native-renderer\codex\game-slot.ps1'
if ((Get-Content "$runs\codex-game.lock" -Raw).Trim().StartsWith('user')) { throw 'User lock: code only.' }
$root="$runs\codex-338\$(Get-Date -Format yyyyMMdd-HHmmss)-$Mode-$Api-$Scale-$Repeat"
New-Item -ItemType Directory -Path $root -Force | Out-Null
$build=Join-Path $workspace 'kk\out\build\kk-dev-vulkan'
Get-ChildItem $build -File | Where-Object { $_.Name -eq 'king_kong.exe' -or $_.Extension -eq '.dll' } |
    Copy-Item -Destination $root
Copy-Item "$build\glyphs" -Destination $root -Recurse
Copy-Item 'F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc\bin\x64\dxcompiler.dll' -Destination $root
Copy-Item 'F:\KK-native-renderer\pipelines\kkpipelines-16235.list' "$root\kkpipelines.list"
robocopy 'F:\KK-native-renderer\userdata' "$root\userdata" /E /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'Private user data copy failed.' }
robocopy 'F:\KK-native-renderer\runs\claude-gpu-setup-20261018-003733\cache' "$root\cache" /E /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'Private cache copy failed.' }
@'
kk_settings_version = 3
kk_setup_version = "2.0.0-beta.14"
kk_shaders_prepared = "2.0.0-beta.14"
kk_original_look = false
kk_frame_rate = 0
kk_render_quality = "quality"
swap_post_effect = "fxaa"
anisotropic_override = 3
ao_mode = 0
ao_strength = 3
native_ao_normal_smoothing = 2
native_ao_quality = "auto"
native_ao_samples = 2
native_gi = "medium"
native_gi_distance = "medium"
native_shadow_quality = "original"
native_water_reflections = "original"
native_volumetrics = "off"
'@ | Set-Content "$root\king_kong.toml" -Encoding ascii
$baseline='native_upscaler=dlss native_upscaler_quality=quality anisotropic_override=3 ao_mode=0 native_ao_quality=auto native_gi=medium native_gi_distance=medium native_shadow_quality=original native_water_reflections=original native_volumetrics=off'
$pairs=@(
 @('gi', 'native_gi=medium native_gi_distance=medium', 'native_gi=low native_gi_distance=near'),
 @('ao', 'ao_mode=0', 'ao_mode=1'),
 @('shadows', 'native_shadow_quality=original', 'native_shadow_quality=high'),
 @('water', 'native_water_reflections=original', 'native_water_reflections=ssr'),
 @('shafts', 'native_volumetrics=off', 'native_volumetrics=low'),
 @('medium', 'native_upscaler_quality=quality', 'native_upscaler_quality=balanced'),
 @('filter', 'anisotropic_override=3', 'anisotropic_override=2'),
 @('deck', 'native_gi=low native_gi_distance=near anisotropic_override=2 native_upscaler_quality=quality',
             'native_gi=low native_gi_distance=near anisotropic_override=2 native_upscaler_quality=performance'),
 @('low', 'native_gi=low native_gi_distance=near anisotropic_override=2 native_upscaler_quality=performance',
            'native_gi=low native_gi_distance=near anisotropic_override=2 native_upscaler_quality=ultra_performance')
)
$plan=[Collections.Generic.List[string]]::new()
if ($Mode -eq 'matrix') {
  foreach ($pair in $pairs) {
    # Three separate launches (-Repeat 1, 2, 3): A B, B A, A B.
    foreach ($side in $(if ($Repeat -eq 2) { @('B','A') } else { @('A','B') })) {
      $value=if ($side -eq 'A') { $pair[1] } else { $pair[2] }
      $plan.Add("$($pair[0])-$side-$Repeat 6 40 1 $baseline $value")
    }
  }
} elseif ($Mode -eq 'focused') {
  # Uncapped repeat with quality/quality controls enclosing each A/B trial.
  # Keep the world's normal clock; a held clock is for captures only.
  for ($trial=1; $trial -le 3; ++$trial) {
    $plan.Add("control-A-$trial 6 40 1 $baseline native_upscaler_quality=quality")
    foreach ($side in $(if ($trial -eq 2) { @('B','A') } else { @('A','B') })) {
      $q=if ($side -eq 'A') { 'quality' } else { 'balanced' }
      $plan.Add("medium-$side-$trial 6 40 1 $baseline native_upscaler_quality=$q")
    }
    $plan.Add("control-B-$trial 6 40 1 $baseline native_upscaler_quality=quality")
  }
} elseif ($Mode -eq 'capture') {
  $snapshots=@(
    @('low-before','native_gi=low native_gi_distance=near anisotropic_override=2 native_upscaler_quality=performance'),
    @('low-after','native_gi=low native_gi_distance=near anisotropic_override=2 native_upscaler_quality=performance'),
    @('medium-before','native_upscaler_quality=quality'),
    @('medium-after',"native_upscaler_quality=$Quality"),
    @('deck-before','native_gi=low native_gi_distance=near anisotropic_override=2 native_upscaler_quality=quality'),
    @('deck-after',"native_gi=low native_gi_distance=near anisotropic_override=2 native_upscaler_quality=$DeckAfter")
  )
  foreach ($p in $snapshots) { $plan.Add("$($p[0]) 1.5 0.1 1 $baseline $($p[1])") }
} else { $plan.Add('scale 6 20 1 native_upscaler=off') }
$plan | Set-Content "$root\plan.txt" -Encoding ascii
Get-ChildItem Env: | Where-Object { $_.Name -like 'KK_DEV_*' -or $_.Name -like 'REX_DEV_*' } |
    ForEach-Object { [Environment]::SetEnvironmentVariable($_.Name,$null,'Process') }
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'
$env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
$env:KK_DEV_AUTOSKIP='1'
$env:KK_DEV_SCRIPT='0:SKIP/4'
$env:KK_DEV_GFX_FINISH="$root\plan.txt,$root\captures"
$env:KK_DEV_GFX_FINISH_START='12'
if ($Mode -eq 'capture') {
  # Bounded capture only: hold the same world moment without using this clock for timing.
  $env:KK_DEV_FORCE_DT='0.000001'
  $env:KK_DEV_FORCE_DT_AT='12'
  $env:KK_DEV_GFX_FINISH_START='14'
}
$args=@('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
 '--kk_startup_child=true',
 '--kk_launcher=false',"--kk_start=chapter:0:$Chapter", "--native_backend=$Api",
 '--kk_check_updates=false','--kk_online_count=false','--kk_discord_status=false','--kk_notify_downloads=false',
 "--config=$root\king_kong.toml","--user_data_root=$root\userdata","--cache_root=$root\cache",
 "--game_data_root=$workspace/kk/assets","--log_file=$root\run.log",'--native_upscaler_output=1920x1080',
 '--native_ao_gpu_time=true','--native_gi_gpu_time=true','--native_upscaler_gpu_time=true','--native_lighting_gpu_time=true')
if ($Mode -in 'matrix','capture') { $args+=@('--native_upscaler=dlss','--kk_frame_rate=60') }
elseif ($Mode -eq 'focused') { $args+=@('--native_upscaler=dlss','--kk_frame_rate=0') }
elseif ($Mode -eq 'scale') { $args+=@('--native_upscaler=off',"--native_render_scale=$Scale") }
else {
  foreach ($name in 'KK_DEV_AUTOSKIP','KK_DEV_SCRIPT','KK_DEV_FORCE_DT','KK_DEV_FORCE_DT_AT',
                   'KK_DEV_GFX_FINISH','KK_DEV_GFX_FINISH_START') {
    [Environment]::SetEnvironmentVariable($name,$null,'Process')
  }
  $args+=@('--native_upscaler=dlss',"--native_upscaler_quality=$Quality",'--kk_benchmark=true',
    '--kk_benchmark_passes=1',
    '--kk_benchmark_route=F:\KK-native-renderer\testbuilds\bench-routes\route-45s-1015-1609.kkbr')
}
@{args=$args;exe_sha256=(Get-FileHash "$root\king_kong.exe").Hash;
 dll_sha256=(Get-FileHash "$root\rexgpu-native.dll").Hash} | ConvertTo-Json -Depth 4 | Set-Content "$root\launch.json"
$game=$null
$helper=$null
try {
  while ((Get-Date) -lt $NotBefore) { Start-Sleep -Seconds 20 }
  if ((Test-Path "$runs\codex-game.priority") -and
      (Get-Content "$runs\codex-game.priority" -Raw).Trim() -notmatch $job) {
    throw 'Another task has game priority; no claim.'
  }
  $deadline=(Get-Date).AddHours(2)
  do {
    if ($Mode -eq 'capture') {
      & powershell -ExecutionPolicy Bypass -File $slot -Job $job -WaitMinutes 3
    } else {
      & powershell -ExecutionPolicy Bypass -File $slot -Job $job -Perf -WaitMinutes 3
    }
    $claim=$LASTEXITCODE
    if ($claim -eq 3) { throw 'User lock: code only.' }
    if ($claim -ne 0) {
      if ((Test-Path "$runs\perf-quiet") -and (Get-Content "$runs\perf-quiet" -Raw).StartsWith("$job ")) {
        & powershell -ExecutionPolicy Bypass -File $slot -Job $job -Release
        # A failed quiet window still leaves time for the other tasks.
        for ($pause=0; $pause -lt 20; ++$pause) { Start-Sleep -Seconds 30 }
      } else { Start-Sleep -Seconds 20 }
    }
  } while ($claim -ne 0 -and (Get-Date) -lt $deadline)
  if ($claim -ne 0) { throw 'Performance slot unavailable. Run directory prepared only.' }
  # Recheck before every launch, after acquiring the slot.
  $others=@(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'")
  $others | Select-Object ProcessId,ExecutablePath | ConvertTo-Json | Set-Content "$root\other-games.json"
  foreach ($p in $others) {
    if (!$p.ExecutablePath -or $p.ExecutablePath -notmatch '^F:\\(KK-native-renderer\\(runs|wt)\\|kk-native-coop\\)') {
      throw 'Owner or unidentifiable game is running; no launch.'
    }
  }
  $helperPath='F:\KK-native-renderer\tools\kkbg\kkbg2.exe'
  if (!(Get-Process kkbg2 -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $helperPath })) {
    $helper=Start-Process $helperPath -ArgumentList "$runs F:\KK-native-renderer\wt" -WindowStyle Hidden -PassThru
  }
  if (!(Get-Process kkbg2 -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $helperPath })) { throw 'kkbg2 is required.' }
  $game=Start-Process "$root\king_kong.exe" -ArgumentList $args -WorkingDirectory $root -WindowStyle Hidden -PassThru
  "RUN $root PID $($game.Id)"
  $watch=[Diagnostics.Stopwatch]::StartNew()
  $otherCheck=-30.0
  $limit=if ($Mode -in 'matrix','focused') { 1500 } elseif ($Mode -eq 'benchmark') { 600 } else { 180 }
  while (!$game.HasExited -and $watch.Elapsed.TotalSeconds -lt $limit) {
    if ($watch.Elapsed.TotalSeconds - $otherCheck -ge 10) {
      $otherCheck=$watch.Elapsed.TotalSeconds
      if ($Mode -ne 'capture' -and (!(Test-Path "$runs\perf-quiet") -or
          !(Get-Content "$runs\perf-quiet" -Raw).StartsWith("$job "))) {
        'Quiet window ownership changed; ending our run.'
        break
      }
      $current=@(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'" | Where-Object { $_.ProcessId -ne $game.Id })
      @{seconds=$otherCheck;games=@($current | Select-Object ProcessId,ExecutablePath)} |
          ConvertTo-Json -Compress -Depth 4 | Add-Content "$root\other-games-timeline.jsonl"
      if ((Get-Content "$runs\codex-game.lock" -Raw).Trim().StartsWith('user') -or
          @($current | Where-Object { !$_.ExecutablePath -or $_.ExecutablePath -notmatch '^F:\\(KK-native-renderer\\(runs|wt)\\|kk-native-coop\\)' }).Count) {
        'Owner or unidentified game appeared; ending our run.'
        break
      }
    }
    $finished=if ($Mode -eq 'benchmark') { 'KK benchmark: complete, status' } else { 'KK gfx finish: done' }
    if ((Test-Path "$root\run.log") -and (Select-String -Path "$root\run.log" -SimpleMatch $finished -Quiet)) { break }
    Start-Sleep -Seconds 2
  }
  "END elapsed=$([math]::Round($watch.Elapsed.TotalSeconds,1)) exited=$($game.HasExited)"
} finally {
  if ($game -and !$game.HasExited) {
    $null=$game.CloseMainWindow()
    if (!$game.WaitForExit(10000)) { Stop-Process -Id $game.Id; $game.WaitForExit() }
  }
  if ($helper -and !$helper.HasExited) { Stop-Process -Id $helper.Id; $helper.WaitForExit() }
  & powershell -ExecutionPolicy Bypass -File $slot -Job $job -Release
}
"EVIDENCE $root"
