param([string]$Label = 'before-vulkan', [string]$Backend = 'vulkan', [switch]$Original, [switch]$Ground, [switch]$Uncapped, [string[]]$Extra = @())
. (Join-Path $PSScriptRoot "run-storage.ps1")
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path (Join-Path  '..\..')).Path
$lockPath = 'F:\KK-native-renderer\runs\codex-game.lock'
if (!(Test-Path -LiteralPath $lockPath)) { Set-Content -LiteralPath $lockPath -Value 'free' }
while ((Get-Content -LiteralPath $lockPath -Raw).Trim() -ne 'free') { Start-Sleep -Seconds 30 }
$ours = Get-Process king_kong -ErrorAction SilentlyContinue | Where-Object { $_.Path -like 'F:\KK-native-renderer\runs\*' -or $_.Path -like 'F:\KK-native-renderer\wt\*' }
if ($ours) { throw 'Another project game is running.' }
if (!(Get-Process kkbg2 -ErrorAction SilentlyContinue)) { throw 'kkbg2 must be running before this test.' }
$run = "F:\KK-native-renderer\runs\task009-$Label-$(Get-Date -Format yyyyMMdd-HHmmss)"
New-Item -ItemType Directory -Path $run | Out-Null
$build = "$workspace\kk\out\build\kk-dev-vulkan"
Initialize-RunBuild -Build $build -Run $run -ShaderPacks @(Get-ChildItem 'F:\KK-native-renderer\wt\beta-upscalers\kk\out\build\kk-dev-vulkan\kkshaders-*.pack' | ForEach-Object FullName) -Dxc 'F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc\bin\x64\dxcompiler.dll'
robocopy F:\KK-native-renderer\userdata "$run\userdata" /E /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'User data copy failed.' }
Get-ChildItem Env: | Where-Object { $_.Name -like 'KK_DEV_*' -or $_.Name -like 'REX_DEV_*' } | ForEach-Object { Remove-Item "Env:$($_.Name)" }
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = '0'
$env:SDL_WINDOW_ACTIVATE_WHEN_RAISED = '0'
$env:KK_DEV_AUTOSKIP = '1'
$env:KK_DEV_SCRIPT = '4:LT/207,5:RT/205,10:RSDOWN/0.4,65:RSRIGHT/0.5,125:RSLEFT/0.7,185:RSUP/0.3'
# Attempt ground impacts with a held stick and a pre-fire capture. Verify the
# captured view: sustained firing can still carry the aim skyward.
if ($Ground) { $env:KK_DEV_SCRIPT = '4:LT+RSDOWN/207,10:RT/200,65:RSRIGHT/0.5,125:RSLEFT/0.7' }
$env:KK_DEV_SHOTS = '4,12,60,120,180,209,211'
if ($Ground) { $env:KK_DEV_SHOTS = '4,8,12,60,60.1,60.2,60.3,60.4,120,180,209,211' }
$env:KK_DEV_SHOTS_FROM = 'menu'
$env:KK_DEV_SHOTS_DIR = $run
$env:KK_DEV_DT_LOG = '0,212'
$env:KK_DEV_GUNFIRE = '1'
$args = @('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
 '--kk_start=chapter:0:8','--kk_launcher=false','--kk_cheats=true','--kk_cheat_ammo=true','--kk_cheat_machine_gun=true',
 "--user_data_root=$run\userdata","--cache_root=$run\cache",'--game_data_root=F:/KK-native-renderer/wt/codex-perf/kk/assets',"--log_file=$run\run.log",
 '--native_debug=32768',"--native_backend=$Backend")
if ($Original) { $args += '--kk_original_look=true' }
if ($Ground) { $args += '--mnk_mode=false','--kk_camera_sensitivity=400' }
if ($Uncapped) { $args += '--kk_frame_rate=0' }
$args += $Extra
Set-Content "$run\test.json" (@{label=$Label;backend=$Backend;classic=$false;script=$env:KK_DEV_SCRIPT;args=$args} | ConvertTo-Json)
$game = $null
# Copying the run takes time. Recheck the shared lock immediately before launch.
while ((Get-Content -LiteralPath $lockPath -Raw).Trim() -ne 'free') { Start-Sleep -Seconds 30 }
$ours = Get-Process king_kong -ErrorAction SilentlyContinue | Where-Object { $_.Path -like 'F:\KK-native-renderer\runs\*' -or $_.Path -like 'F:\KK-native-renderer\wt\*' }
if ($ours) { throw 'Another project game started while preparing the run.' }
Set-Content -LiteralPath $lockPath -Value "task009 $(Get-Date -Format o)"
try {
 $game = Start-Process -FilePath "$run\king_kong.exe" -ArgumentList $args -WorkingDirectory $run -WindowStyle Hidden -PassThru
 Write-Output "RUN $run PID $($game.Id)"
 'seconds,private_bytes,working_set,cpu_seconds' | Set-Content "$run\process.csv"
 $clock = [Diagnostics.Stopwatch]::StartNew()
 while (!$game.HasExited -and $clock.Elapsed.TotalSeconds -lt 285) {
  $game.Refresh()
  "$($clock.Elapsed.TotalSeconds),$($game.PrivateMemorySize64),$($game.WorkingSet64),$($game.TotalProcessorTime.TotalSeconds)" | Add-Content "$run\process.csv"
  if ((Test-Path "$run\shot_2110.png") -or (Test-Path "$run\shot_2110.bmp")) { break }
  Start-Sleep -Seconds 2
 }
} finally {
 if ($game -and !$game.HasExited) {
  $null = $game.CloseMainWindow()
  if (!$game.WaitForExit(15000)) { Stop-Process -Id $game.Id; $game.WaitForExit() }
 }
 Set-Content -LiteralPath $lockPath -Value 'free'
}
Assert-RunBuildUnchanged $run
  Complete-RunCaptures $run
Write-Output "DONE $run"


