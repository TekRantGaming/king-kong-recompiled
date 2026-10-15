param(
 [Parameter(Mandatory)][string]$Label,
 [ValidateSet('vulkan','d3d12')][string]$Api='vulkan',
 [int]$Cap=0,
 [string]$Build='F:\KK-native-renderer\wt\bench-rec\kk\out\build\kk-dev-vulkan'
)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\game-slot.ps1"
$task='task116'
$ledger='F:\KK-native-renderer\runs\task116-launches.txt'
if ((Test-Path -LiteralPath $ledger) -and @(Get-Content -LiteralPath $ledger).Count -ge 8) { throw 'Eight-run limit reached' }
$run="F:\KK-native-renderer\runs\task116-$Label-"+(Get-Date -Format yyyyMMdd-HHmmss)
New-Item -ItemType Directory -Path $run | Out-Null
$base='F:\KK-native-renderer\testbuilds\bench-rec-1015-1617\KingKong'
& robocopy $base $run /E /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -gt 7) { throw 'package copy failed' }
Copy-Item -LiteralPath "$Build\king_kong.exe","$Build\rexgpu-native.dll","$Build\rexruntime.dll" -Destination $run
$packs='F:\KK-native-renderer\runs\task113-current-vulkan-20261015-151840'
Copy-Item -LiteralPath "$packs\kkshaders-spirv.pack","$packs\kkshaders-dxil.pack" -Destination $run
Copy-Item -LiteralPath 'F:\KK-native-renderer\testbuilds\bench-routes\route-45s-1015-1609.kkbr' -Destination "$run\benchmark-route.kkbr"
& robocopy 'F:\KK-native-renderer\userdata' "$run\userdata" /E /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -gt 7) { throw 'userdata copy failed' }
$gameArgs=@('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
 '--kk_renderer=native',"--native_backend=$Api", "--kk_frame_rate=$Cap",'--vsync=false',
 '--kk_launcher=false','--kk_benchmark=true','--kk_startup_child=true',
 "--user_data_root=$run\userdata",'--game_data_root=F:/KK-native-renderer/wt/bench-rec/kk/assets',
 "--cache_root=$run\cache","--log_file=$run\run.log","--config=$run\king_kong.toml",
 '--kk_check_updates=false','--kk_online_count=false','--kk_discord_status=false',
 '--video_mode_width=1280','--video_mode_height=720','--draw_resolution_scale_x=1','--draw_resolution_scale_y=1',
 '--kk_render_quality=native','--native_upscaler=off','--native_gi=high',
 '--native_ao_method=gtao','--native_ao_samples=3','--ao_mode=1','--ao_strength=3',
 '--native_shadow_quality=original','--native_water_reflections=original','--native_volumetrics=off')
$gameArgs | Set-Content -LiteralPath "$run\arguments.txt"
Get-FileHash -Algorithm SHA256 -LiteralPath "$run\king_kong.exe","$run\rexgpu-native.dll","$run\rexruntime.dll","$run\benchmark-route.kkbr" | ConvertTo-Json | Set-Content -LiteralPath "$run\input-hashes.json"
Get-ChildItem Env: | Where-Object { $_.Name -like 'KK_DEV_*' -or $_.Name -eq 'KK_AUTOPLAY' } | ForEach-Object { [Environment]::SetEnvironmentVariable($_.Name,$null,'Process') }
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'
$env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
$guard=Wait-GameSlot -Task $task -RequireBackgroundHelper
$owned=@()
try {
 $p=Start-Process -FilePath "$run\king_kong.exe" -ArgumentList $gameArgs -WorkingDirectory $run -WindowStyle Hidden -PassThru
 $owned+=$p.Id
 "$Label $Api cap=$Cap pid=$($p.Id) $run" | Add-Content -LiteralPath $ledger
 Write-Output "LAUNCHED $($p.Id) $run"
 $deadline=(Get-Date).AddMinutes(7)
 $lastProgress=''
 $result=$null
 while ((Get-Date) -lt $deadline) {
  $games=@(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'")
  $children=@($games | Where-Object { $_.ParentProcessId -in $owned -and $_.ExecutablePath -eq "$run\king_kong.exe" })
  foreach ($c in $children) { if ($owned -notcontains $c.ProcessId) { $owned+=$c.ProcessId } }
  if ($games | Where-Object { $_.ProcessId -notin $owned }) { throw 'Another game appeared; stopping only this test process' }
  $result=Get-ChildItem -LiteralPath "$run\userdata\benchmarks" -Filter frame-times.svg -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($result) { break }
  if ($p.HasExited) { throw "replay exited before results: $run" }
  if (Test-Path -LiteralPath "$run\run.log") {
   $progress=(Select-String -LiteralPath "$run\run.log" -Pattern 'KK benchmark replay: chapter|KK benchmark: complete|KK benchmark: saving|KK play: uni_go loads level' | Select-Object -Last 1).Line
   if ($progress -and $progress -ne $lastProgress) { Write-Output $progress; $lastProgress=$progress }
  }
  Start-Sleep -Seconds 2
 }
 if (!$result) { throw "no results within seven minutes: $run" }
 $resultDir=$result.DirectoryName
 Get-Content -LiteralPath "$resultDir\results.txt" | Select-Object -First 175
 [pscustomobject]@{Run=$run;Pid=$p.Id;OwnedPids=$owned;Results=$resultDir;Api=$Api;Cap=$Cap} | ConvertTo-Json | Set-Content -LiteralPath "$run\test-result.json"
} finally {
 foreach ($own in $owned) { $proc=Get-Process -Id $own -ErrorAction SilentlyContinue; if ($proc) { Stop-Process -Id $own; Wait-Process -Id $own -ErrorAction SilentlyContinue } }
 Release-GameSlot $guard
}
