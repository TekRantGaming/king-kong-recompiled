param([ValidateSet('crash','launcher')][string]$Mode='crash', [string]$PreviousRun='')
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$lockPath='F:\KK-native-renderer\runs\codex-game.lock'
$run="F:\KK-native-renderer\runs\task043-$Mode-$(Get-Date -Format yyyyMMdd-HHmmss)"
$build=Join-Path $workspace 'kk/out/build/kk-dev-vulkan'
New-Item -ItemType Directory -Path $run | Out-Null
Get-ChildItem -LiteralPath $build -File | Where-Object { $_.Extension -in '.exe','.dll' } | Copy-Item -Destination $run
Copy-Item -LiteralPath "$build\glyphs" -Destination $run -Recurse
# Reuse existing locally built game shaders, never download game-derived data.
$pack='F:\KK-native-renderer\runs\task015-after-auto-e7-ao1-20261013-175100\kkshaders-spirv.pack'
Copy-Item -LiteralPath $pack -Destination $run
Copy-Item -LiteralPath 'F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc\bin\x64\dxcompiler.dll' -Destination $run
$sourceData='F:\KK-native-renderer\userdata'
if ($Mode -eq 'launcher') {
  if (!$PreviousRun.StartsWith('F:\KK-native-renderer\runs\task043-crash-') -or !(Test-Path -LiteralPath "$PreviousRun\userdata\crashes")) {
    throw 'Launcher test requires this task''s private preceding crash run.'
  }
  $sourceData="$PreviousRun\userdata"
}
robocopy $sourceData "$run\userdata" /E /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'User data copy failed.' }
Get-ChildItem Env: | Where-Object { $_.Name -like 'KK_DEV_*' -or $_.Name -like 'REX_DEV_*' } | ForEach-Object { Remove-Item "Env:$($_.Name)" }
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'
$env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
$env:KK_AUTOPLAY='3600' # suppress update/pack downloads; no autoplay in this two-minute test
$arguments=@('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
 "--user_data_root=$run\userdata","--game_data_root=$($workspace.Replace('\','/'))/kk/assets","--log_file=$run\run.log",
 "--cache_root=$run\cache",'--native_backend=vulkan','--native_launcher_prewarm=false','--kk_check_updates=false',
 '--kk_online_count=false','--kk_online_asked=true','--kk_discord_status=false','--kk_launcher_music=false',
 '--kk_launcher_sounds=false','--kk_last_version=2.0.0-beta.3','--kk_original_look=true',
 '--kk_original_look_keep=native_upscaler,ao_mode','--native_upscaler=off','--ao_mode=0')
if ($Mode -eq 'crash') {
  $env:KK_DEV_AUTOSKIP='1'
  $env:KK_DEV_SCRIPT='0:SKIP/120'
  $env:KK_DEV_CRASH_AFTER='120'
  $arguments+='--kk_start=chapter:0:8','--kk_launcher=false'
} else {
  $env:KK_DEV_DIAGNOSTICS='1'
  $arguments+='--kk_launcher=true'
}
@{mode=$Mode;previous=$PreviousRun;args=$arguments;pack=$pack;exe_sha256=(Get-FileHash "$run\king_kong.exe").Hash} |
  ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$run\test.json"
if (!(Test-Path -LiteralPath $lockPath)) { Set-Content -LiteralPath $lockPath 'free' }
while ($true) {
  $state=(Get-Content -LiteralPath $lockPath -Raw).Trim()
  if ($state.StartsWith('user')) { Write-Output 'USER LOCK: code-only, no game launched'; exit 3 }
  $games=@(Get-Process king_kong -ErrorAction SilentlyContinue)
  $userGame=@($games | Where-Object { !$_.Path -or ($_.Path -notlike 'F:\KK-native-renderer\runs\*' -and $_.Path -notlike 'F:\KK-native-renderer\wt\*') })
  if ($userGame.Count) {
    Write-Output 'Waiting for the user game to close; leaving it untouched.'
    Start-Sleep -Seconds 60
    continue
  }
  if ($state -eq 'free' -and !$games.Count) { break }
  Write-Output "Waiting for game lock: $state"
  Start-Sleep -Seconds 30
}
if (!(Get-Process kkbg2 -ErrorAction SilentlyContinue)) { throw 'kkbg2 must be running; leave it running after tests.' }
Set-Content -LiteralPath $lockPath "task043 $(Get-Date -Format o)"
$game=$null
try {
  $game=Start-Process -FilePath "$run\king_kong.exe" -ArgumentList $arguments -WorkingDirectory $run -WindowStyle Hidden -PassThru
  Write-Output "RUN $run PID $($game.Id)"
  $clock=[Diagnostics.Stopwatch]::StartNew()
  $limit=if ($Mode -eq 'crash') {180} else {120}
  while (!$game.HasExited -and $clock.Elapsed.TotalSeconds -lt $limit) { Start-Sleep -Seconds 2 }
  if ($Mode -eq 'crash' -and !$game.HasExited) { throw 'Forced crash did not exit within 180 seconds.' }
} finally {
  if ($game -and !$game.HasExited) {
    $null=$game.CloseMainWindow()
    if (!$game.WaitForExit(15000)) { Stop-Process -Id $game.Id; $game.WaitForExit() }
  }
  Set-Content -LiteralPath $lockPath 'free'
}
@{pid=$game.Id;exit_code=$game.ExitCode;elapsed_seconds=$clock.Elapsed.TotalSeconds} |
  ConvertTo-Json | Set-Content -LiteralPath "$run\result.json"
Write-Output "DONE $run"
