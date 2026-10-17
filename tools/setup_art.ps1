# The setup's comparison close-ups (job J4, codex/state/setup-flow.md "Comparison pictures").
#
# The pictures come from the player's copy of the game, so they are never committed: this developer script
# captures them in the game and writes them into a release package's setup-art folder (beside king_kong.exe),
# where the launcher's graphics setup pages load them (<key>-original.png and <key>-new.png, 16:9 crops).
#
# Each launch starts a chapter straight away (--kk_start, no launcher), skips its opening, stands still, and
# then changes the settings live phase by phase (KK_DEV_GFX_FINISH, the same changes as the F8 panel),
# capturing the game's own frame after each (guest output only, never the desktop). Game runs follow
# codex/GAME-RULES.md: a normal game slot per launch (codex/game-slot.ps1), windowed 1280x720, activation
# hints off, a private copy of the program and the test user data under runs\, only our own processes stopped.
# The frames are drawn at 2560x1440 (native_frame_size / native_upscaler_output) whatever the window's size.
#
# Usage (a kk-dev-* build: it has the developer aids):
#   powershell -ExecutionPolicy Bypass -File tools\setup_art.ps1 -Build <kk\out\build\kk-dev-vulkan>
#       -GameRoot <kk\assets> -Out <package>\setup-art [-Launches main,low,orig,water] [-Job claude-launcher]
# then tools\setup_art_crop.py <the runs printed> <out> crops and pairs them (setup_art_crops.json).
param(
  [Parameter(Mandatory)][string]$Build,
  [Parameter(Mandatory)][string]$GameRoot,
  [string]$Launches = 'main,low,orig,water',
  [string]$Job = 'claude-launcher-art',
  [string]$CacheFrom = '',           # a run folder whose cache\ (prepared shaders) is copied in: faster, no pop-in
  [string]$UserData = 'F:\KK-native-renderer\userdata',
  [int]$Timeout = 300,
  [int]$Entry = -1,                  # another chapter-select entry for main, low, orig and probe
  [int]$WaterEntry = -1              # another one for water
)
$ErrorActionPreference = 'Stop'
$runs = 'F:\KK-native-renderer\runs'

# What every phase starts from: the new effects off, no upscaler, no extra anti-aliasing or sharpening.
$off = 'native_upscaler=off swap_post_effect=none native_sharpness=off anisotropic_override=-1 ao_mode=0 native_gi=off ' +
       'native_shadow_quality=original native_water_reflections=original native_volumetrics=off'
# <name> <settle s> <measure s> <captures> <cvar>=<value> ... (debug_overlay.cpp FinishTick)
$hi = @('--native_frame_size=2560x1440', '--native_upscaler_output=2560x1440', '--present_effect=bilinear',
        '--kk_original_look=false')
$defs = [ordered]@{
  # Chapter-select entry 4: a jungle clearing at dusk, the spiked gate against the sky (most options).
  main  = @{ entry = 4; args = $hi + @('--native_upscaler=dlss', '--native_upscaler_quality=performance'); plan = @(
      "ref 3 0.3 1 $off",
      "gi 3 0.3 1 native_gi=high",
      "ao 3 0.3 1 native_gi=off ao_mode=1 ao_strength=3",
      "smaa 2 0.3 1 ao_mode=0 swap_post_effect=smaa",
      "sharpening 2 0.3 1 swap_post_effect=none native_sharpness=high",
      "filtering 2 0.3 1 native_sharpness=off anisotropic_override=5",
      "shadows 3 0.3 1 anisotropic_override=-1 native_shadow_quality=ultra",
      "upscaler 4 0.3 1 native_shadow_quality=original native_upscaler=dlss native_upscaler_quality=performance",
      "remastered 4 0.3 1 native_upscaler_quality=quality native_gi=high ao_mode=1 ao_strength=3 anisotropic_override=5",
      "ref2 4 0.3 1 $off") }
  # The same spot drawn at 1280x720 with plain scaling (the Original side of Resolution and Upscaler).
  low   = @{ entry = 4; args = @('--native_frame_size=1280x720', '--present_effect=bilinear', '--native_upscaler=off',
                                 '--kk_original_look=false'); plan = @("ref720 3 0.3 1 $off") }
  # The Xbox 360 look (Original look: its own 720p settings).
  orig  = @{ entry = 4; args = @('--kk_original_look=true'); plan = @('original 3 0.3 1') }
  # A quick look at a spot (-Entry): is it still, and do the lighting options show there?
  probe = @{ entry = 4; args = $hi + @('--native_upscaler=off'); plan = @(
      "ref 3 0.3 1 $off", "gi 3 0.3 1 native_gi=high", "ao 3 0.3 1 native_gi=off ao_mode=1 ao_strength=3",
      "shadows 3 0.3 1 ao_mode=0 native_shadow_quality=ultra", "ref2 3 0.3 1 $off") }
  # Chapter-select entry 11: a canyon path between mossy rocks, a pool beyond, mist (water, light shafts).
  water = @{ entry = 11; args = $hi + @('--native_upscaler=off'); plan = @(
      "ref 3 0.3 1 $off",
      "water 3 0.3 1 native_water_reflections=ssr",
      "shafts 3 0.3 1 native_water_reflections=original native_volumetrics=high",
      "ref2 3 0.3 1 $off") }
}

if (!(Get-Process kkbg2 -ErrorAction SilentlyContinue)) { throw 'kkbg2 must be running (it keeps test windows behind)' }
$done = @()
foreach ($name in ($Launches -split ',')) {
  $d = $defs[$name]
  if (!$d) { throw "unknown launch $name" }
  if ($Entry -ge 0 -and $name -ne 'water') { $d.entry = $Entry }
  if ($WaterEntry -ge 0 -and $name -eq 'water') { $d.entry = $WaterEntry }
  $run = "$runs\setup-art-$name-$(Get-Date -Format yyyyMMdd-HHmmss)"
  New-Item -ItemType Directory -Path "$run\captures" -Force | Out-Null
  Get-ChildItem -LiteralPath $Build -File | Where-Object { $_.Name -eq 'king_kong.exe' -or $_.Extension -eq '.dll' } |
    Copy-Item -Destination $run
  foreach ($f in 'dxcompiler.dll', 'dxil.dll') {
    $p = "F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc\bin\x64\$f"
    if (Test-Path $p) { Copy-Item -LiteralPath $p -Destination $run }
  }
  if ($CacheFrom) { robocopy "$CacheFrom\cache" "$run\cache" /E /NFL /NDL /NJH /NJS /NP | Out-Null }
  robocopy $UserData "$run\userdata" /E /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) { throw 'private user data copy failed' }
  $d.plan | Set-Content -LiteralPath "$run\plan.txt" -Encoding ascii
  Get-ChildItem Env: | Where-Object { $_.Name -like 'KK_*' -or $_.Name -like 'REX_DEV_*' } |
    ForEach-Object { [Environment]::SetEnvironmentVariable($_.Name, $null, 'Process') }
  $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = '0'; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED = '0'
  # Skip the chapter's opening (Start through its movies and cutscene, then Start and B open and close the
  # game's own pause), then stand still; the phases start 40 s after the level has loaded.
  $env:KK_DEV_AUTOSKIP = '1'
  $env:KK_DEV_SCRIPT = '4:START,8:START,12:START,16:START,20:START,24:START,28:B,30:B'
  $env:KK_DEV_GAME_PAUSE = '1'
  $env:KK_DEV_GFX_FINISH = "$run\plan.txt,$run\captures"
  $env:KK_DEV_GFX_FINISH_START = '40'
  $launch = @('--fullscreen=false', '--window_width=1280', '--window_height=720', '--kk_fullscreen_mode=desktop',
              '--kk_launcher=false', "--kk_start=chapter:0:$($d.entry)", '--gpu_plugin=native', '--native_backend=vulkan',
              '--kk_frame_rate=30', "--user_data_root=$run\userdata", "--game_data_root=$GameRoot", "--log_file=$run\run.log",
              "--cache_root=$run\cache", "--config=$run\king_kong.toml", '--kk_check_updates=false',
              '--kk_online_count=false', '--kk_discord_status=false', '--native_validation=false') + $d.args
  $launch | Set-Content -LiteralPath "$run\arguments.txt"
  $claim = & powershell -ExecutionPolicy Bypass -File F:\KK-native-renderer\codex\game-slot.ps1 -Job $Job
  if ("$claim" -notmatch 'CLAIMED') { Write-Output "NO SLOT ($claim): stopped before $name"; break }
  $owned = [Collections.Generic.HashSet[int]]::new()
  $finished = $false
  $timer = [Diagnostics.Stopwatch]::StartNew()
  try {
    $game = Start-Process -FilePath "$run\king_kong.exe" -ArgumentList $launch -WorkingDirectory $run -WindowStyle Hidden `
      -PassThru -RedirectStandardOutput "$run\stdout.log" -RedirectStandardError "$run\stderr.log"
    [void]$owned.Add($game.Id)
    while ($timer.Elapsed.TotalSeconds -lt $Timeout) {
      foreach ($c in @(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'" |
                       Where-Object { $_.ExecutablePath -eq "$run\king_kong.exe" })) { [void]$owned.Add([int]$c.ProcessId) }
      $log = ''
      try { $s = [IO.File]::Open("$run\run.log", 'Open', 'Read', 'ReadWrite'); $r = [IO.StreamReader]::new($s); $log = $r.ReadToEnd(); $r.Dispose() } catch {}
      if ($log -match 'KK gfx finish: done') { $finished = $true; Start-Sleep -Seconds 2; break }
      if (!@($owned | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue }).Count) { break }
      Start-Sleep -Seconds 2
    }
  } finally {
    foreach ($id in $owned) {  # only our own processes: this run folder's program
      $p = Get-Process -Id $id -ErrorAction SilentlyContinue
      if ($p -and $p.Path -eq "$run\king_kong.exe") { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
    }
    foreach ($id in $owned) { $p = Get-Process -Id $id -ErrorAction SilentlyContinue; if ($p) { $p.WaitForExit(10000) | Out-Null } }
    & powershell -ExecutionPolicy Bypass -File F:\KK-native-renderer\codex\game-slot.ps1 -Job $Job -Release | Out-Null
  }
  $left = @($owned | Where-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue }).Count
  $shots = @(Get-ChildItem -LiteralPath "$run\captures" -Filter '*.png' -ErrorAction SilentlyContinue).Count
  Write-Output ("LAUNCH {0} finished={1} seconds={2:N0} captures={3} still_running={4} {5}" -f $name, $finished,
                $timer.Elapsed.TotalSeconds, $shots, $left, $run)
  $done += $run
}
Write-Output ("RUNS " + ($done -join ' '))
