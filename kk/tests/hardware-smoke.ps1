param([ValidateSet('first', 'existing')][string]$Case = 'first')
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$lock = 'F:\KK-native-renderer\runs\codex-game.lock'
function Wait-Free {
  if (!(Test-Path -LiteralPath $lock)) { Set-Content -LiteralPath $lock 'free' }
  while ($true) {
    $state = (Get-Content -LiteralPath $lock -Raw).Trim()
    if ($state.StartsWith('user')) { throw 'User lock: game tests prohibited for this turn.' }
    # Standing decisions prohibit a second game, including alongside the user copy.
    if ($state -eq 'free' -and !(Get-Process king_kong -ErrorAction SilentlyContinue)) { break }
    Start-Sleep -Seconds 30
  }
  if (!(Get-Process kkbg2 -ErrorAction SilentlyContinue)) { throw 'kkbg2 must already be running.' }
}
# Check user lock before preparing any game run.
if ((Get-Content -LiteralPath $lock -Raw).Trim().StartsWith('user')) { throw 'User lock: no game tests.' }
$run = "F:\KK-native-renderer\runs\task038-$Case-$(Get-Date -Format yyyyMMdd-HHmmss)"
New-Item -ItemType Directory -Path $run | Out-Null
$build = Join-Path $workspace 'kk/out/build/kk-dev-vulkan'
Get-ChildItem -LiteralPath $build -File | Where-Object { $_.Extension -in '.exe', '.dll' } | Copy-Item -Destination $run
Copy-Item -LiteralPath "$build\glyphs" -Destination $run -Recurse
Copy-Item -LiteralPath 'F:\KK-native-renderer\test-shaders\tour\kkshaders-spirv.pack' -Destination $run
Copy-Item -LiteralPath 'F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc\bin\x64\dxcompiler.dll' -Destination $run
robocopy F:\KK-native-renderer\userdata "$run\userdata" /E /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'Private user-data copy failed.' }
if ($Case -eq 'existing') {
  @'
kk_settings_version = 2
kk_render_quality = "balanced"
native_upscaler = "fsr3"
native_upscaler_quality = "balanced"
native_ao_quality = "half"
native_ao_samples = 1
ao_mode = 0
kk_frame_rate = 30
native_backend = "auto"
'@ | Set-Content -LiteralPath "$run\king_kong.toml"
}
Get-ChildItem Env: | Where-Object { $_.Name -like 'KK_DEV_*' -or $_.Name -like 'REX_DEV_*' } |
    ForEach-Object { [Environment]::SetEnvironmentVariable($_.Name, $null, 'Process') }
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = '0'
$env:SDL_WINDOW_ACTIVATE_WHEN_RAISED = '0'
$env:KK_DEV_SCRIPT = '0:SKIP/120'
$env:KK_DEV_SHOTS = '100,120'
$env:KK_DEV_SHOTS_FROM = 'menu'
$env:KK_DEV_SHOTS_DIR = $run
$args = @('--fullscreen=false', '--window_width=1280', '--window_height=720', '--kk_fullscreen_mode=desktop',
    '--kk_launcher=false', '--kk_start=chapter:0:8', '--kk_check_updates=false', '--kk_online_count=false',
    '--kk_discord_status=false', "--user_data_root=$run\userdata", "--game_data_root=$($workspace.Replace('\','/'))/kk/assets",
    "--log_file=$run\run.log", "--cache_root=$run\cache")
@{case=$Case; args=$args; dll_sha256=(Get-FileHash "$run\rexgpu-native.dll").Hash;
  exe_sha256=(Get-FileHash "$run\king_kong.exe").Hash} | ConvertTo-Json | Set-Content "$run\test.json"
Wait-Free
Set-Content -LiteralPath $lock "task038 $(Get-Date -Format o)"
$game = $null
try {
  $game = Start-Process -FilePath "$run\king_kong.exe" -ArgumentList $args -WorkingDirectory $run -WindowStyle Hidden -PassThru
  Write-Output "RUN $run PID $($game.Id)"
  $timer = [Diagnostics.Stopwatch]::StartNew()
  while (!$game.HasExited -and $timer.Elapsed.TotalSeconds -lt 240) {
    if (Test-Path -LiteralPath "$run\shot_1200.bmp") { break }
    Start-Sleep -Seconds 2
  }
  Write-Output "END $($timer.Elapsed.TotalSeconds) seconds; exited=$($game.HasExited); 120-second-frame=$(Test-Path -LiteralPath "$run\shot_1200.bmp")"
} finally {
  if ($game -and !$game.HasExited) {
    $null = $game.CloseMainWindow()
    if (!$game.WaitForExit(15000)) { Stop-Process -Id $game.Id; $game.WaitForExit() }
  }
  Set-Content -LiteralPath $lock 'free'
}
Write-Output "DONE $run"
