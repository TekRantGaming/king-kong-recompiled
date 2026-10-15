param([string]$Build='F:\KK-native-renderer\wt\bench-rec\kk\out\build\kk-dev-vulkan')
$ErrorActionPreference='Stop'
. 'F:\KK-native-renderer\wt\bench-rec\codex\game-slot.ps1'
$task='task115'
$run='F:\KK-native-renderer\runs\task115-record-'+(Get-Date -Format yyyyMMdd-HHmmss)
New-Item -ItemType Directory -Path $run | Out-Null
$base='F:\KK-native-renderer\testbuilds\beta6-test-1015-1441\KingKong'
& robocopy $base $run /E /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -gt 7) { throw 'package copy failed' }
Copy-Item -LiteralPath "$Build\king_kong.exe","$Build\rexgpu-native.dll","$Build\rexruntime.dll" -Destination $run
$packs='F:\KK-native-renderer\runs\task113-current-vulkan-20261015-151840'
Copy-Item -LiteralPath "$packs\kkshaders-spirv.pack","$packs\kkshaders-dxil.pack" -Destination $run
& robocopy 'F:\KK-native-renderer\userdata' "$run\userdata" /E /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -gt 7) { throw 'userdata copy failed' }
$args=@('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
 '--kk_renderer=native','--native_backend=vulkan','--kk_frame_rate=60','--vsync=false',
 '--kk_launcher=false','--kk_benchmark_record=true','--kk_startup_child=true',
 "--user_data_root=$run\userdata",'--game_data_root=F:/KK-native-renderer/wt/bench-rec/kk/assets',
 "--cache_root=$run\cache","--log_file=$run\run.log","--config=$run\king_kong.toml",
 '--kk_check_updates=false','--kk_online_count=false','--kk_discord_status=false',
 '--native_upscaler=off','--native_gi=off','--native_ao_method=off','--ao_mode=0',
 '--native_shadow_quality=original','--native_water_reflections=original','--native_volumetrics=off')
$args | Set-Content -LiteralPath "$run\arguments.txt"
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'
$env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
$env:KK_DEV_BENCH_SECONDS='4'
$env:KK_DEV_AUTOSKIP='1'
$env:KK_DEV_SCRIPT='0:RSRIGHT+LT+RT+X/0.15'
$env:KK_DEV_WANDER='1'
$claim=Try-ClaimGameSlot -Task $task -RequireBackgroundHelper
if (!$claim.Guard) { Write-Output "NO LAUNCH: $($claim.State)"; exit 3 }
$owned=@()
try {
 $p=Start-Process -FilePath "$run\king_kong.exe" -ArgumentList $args -WorkingDirectory $run -WindowStyle Hidden -PassThru
 $owned+= $p.Id
 Write-Output "LAUNCHED $($p.Id) $run"
 $deadline=(Get-Date).AddSeconds(180)
 $route=$null; $child=$null
 while ((Get-Date) -lt $deadline) {
  $children=@(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'" | Where-Object { $_.ParentProcessId -eq $p.Id -and $_.ExecutablePath -eq "$run\king_kong.exe" })
  foreach ($c in $children) { if ($owned -notcontains $c.ProcessId) { $owned+=$c.ProcessId; $child=$c.ProcessId } }
  $route=Get-ChildItem -LiteralPath "$run\userdata\benchmarks" -Filter route.kkbr -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($route -and $child -and $p.HasExited) { break }
  if ($p.HasExited -and !$child -and !$route) { throw 'recorder exited without route' }
  Start-Sleep -Seconds 2
 }
 $log=Get-Content -LiteralPath "$run\run.log" -Raw
 if (!$route) { throw 'no route written before deadline' }
 $raw=Get-Content -LiteralPath $route.FullName -Raw
 if ($raw -notmatch '(?m)^complete 1\r?$') { throw 'route is incomplete' }
 $matches=[regex]::Matches($log,'KK benchmark recording: chapter [1-4] entry')
 if ($matches.Count -ne 4) { throw "expected 4 chapter starts, got $($matches.Count)" }
 if (!$child) { throw 'no return-to-launcher child observed' }
 [pscustomobject]@{Run=$run;Pid=$p.Id;LauncherPid=$child;Route=$route.FullName;Chapters=$matches.Count;Complete=$true} | ConvertTo-Json | Set-Content -LiteralPath "$run\test-result.json"
 Get-Content -LiteralPath "$run\test-result.json"
} finally {
 foreach ($own in $owned) { $proc=Get-Process -Id $own -ErrorAction SilentlyContinue; if ($proc) { Stop-Process -Id $own; Wait-Process -Id $own -ErrorAction SilentlyContinue } }
 Release-GameSlot $claim.Guard
}
