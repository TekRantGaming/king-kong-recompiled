param([ValidateSet('chapter','golden')][string]$Kind='chapter',
 [ValidateSet('small','minimum')][string]$Tables='minimum', [int]$Chapter=8,
 [string]$SceneRun='vrex', [switch]$Gtao, [string]$Upscaler='off')
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$lockPath='F:\KK-native-renderer\runs\codex-game.lock'
$priorityPath='F:\KK-native-renderer\runs\codex-game.priority'
$python='C:\Users\TekRa\AppData\Local\Programs\Python\Python312\python.exe'
$compare=Join-Path $PSScriptRoot 'compare.py'
$root="F:\KK-native-renderer\runs\task098-$Kind-$Tables-$(Get-Date -Format yyyyMMdd-HHmmss)"
if ((Test-Path $lockPath) -and (Get-Content $lockPath -Raw).Trim().StartsWith('user')) {
 Write-Output 'USER LOCK: no launch; code-only work.'; exit 3
}
New-Item -ItemType Directory -Path $root | Out-Null
$run=Join-Path $root 'game'
New-Item -ItemType Directory -Path $run | Out-Null
$build=Join-Path $workspace 'kk/out/build/kk-dev-vulkan'
Get-ChildItem -LiteralPath $build -File | Where-Object { $_.Extension -in '.exe','.dll' } | Copy-Item -Destination $run
Copy-Item -LiteralPath "$build\glyphs" -Destination $run -Recurse
Copy-Item -LiteralPath 'F:\KK-native-renderer\cache\native-shaders\kkshaders-spirv.pack' -Destination $run
Copy-Item -LiteralPath 'F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc\bin\x64\dxcompiler.dll' -Destination $run
robocopy F:\KK-native-renderer\userdata "$run\userdata" /E /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'Private user data copy failed.' }
Get-ChildItem Env: | Where-Object { $_.Name -like 'KK_*' -or $_.Name -like 'REX_DEV_*' -or $_.Name -like 'VK_*' } | ForEach-Object { Remove-Item "Env:$($_.Name)" }
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'
$env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
$env:KK_DEV_SHOTS_DIR=$run
$env:VK_LAYER_PATH='F:\KK-native-renderer\downloads\vulkan-sdk-latest\sdk\Bin'
$env:VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation'
@('khronos_validation.validate_sync = true',
  'khronos_validation.report_flags = info,warn,error',
  'khronos_validation.debug_action = VK_DBG_LAYER_ACTION_LOG_MSG',
  "khronos_validation.log_filename = $run\vulkan-validation.log") | Set-Content "$run\vk_layer_settings.txt"
$env:VK_LAYER_SETTINGS_PATH=$run
if ($Kind -eq 'golden') {
 $planText=& $python -I $compare plan --scenes $SceneRun --plugin native --golden-root F:\KK-native-renderer\golden
 if ($LASTEXITCODE) { throw 'Golden scene plan failed.' }
 $plan=$planText | ConvertFrom-Json
 if ($plan.runs.Count -ne 1) { throw 'Choose one golden run per invocation.' }
 $scene=$plan.runs[0]
 foreach ($kv in $scene.env.PSObject.Properties) { [Environment]::SetEnvironmentVariable($kv.Name,[string]$kv.Value,'Process') }
 $sceneArgs=@($scene.args)
 $limit=[double]$scene.limit_s
 $expected=@($scene.expected)
} else {
 $env:KK_DEV_AUTOSKIP='1'
 $env:KK_DEV_SCRIPT='0:SKIP/120'
 $env:KK_DEV_DT_LOG='0,240'
 $env:KK_DEV_SHOTS='60,120'
 $env:KK_DEV_SHOTS_FROM='menu'
 $sceneArgs=@("--kk_start=chapter:0:$Chapter",'--kk_launcher=false','--kk_frame_rate=30','--kk_original_look=true')
 $limit=240
 $expected=@('shot_1200.png')
}
$expected=@($expected | ForEach-Object { $_ -replace '\.bmp$', '.png' })
$keep='native_upscaler,native_upscaler_quality,native_vk_small_tables,native_vk_minimum_tables'
if ($Gtao) { $keep+=',ao_mode,ao_strength' }
$smallFlag=($Tables -eq 'small').ToString().ToLowerInvariant()
$minimumFlag=($Tables -eq 'minimum').ToString().ToLowerInvariant()
$overrides=@('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
 '--kk_startup_child=true','--native_backend=vulkan','--native_debug=32768','--native_validation=true',
 '--native_pipeline_cache=false','--native_pipeline_list=false','--native_vk_pipeline_cache=false',
 "--native_upscaler=$Upscaler",'--native_upscaler_quality=quality',"--kk_original_look_keep=$keep",
 "--native_vk_small_tables=$smallFlag","--native_vk_minimum_tables=$minimumFlag",
 "--user_data_root=$run\userdata","--game_data_root=$($workspace.Replace('\','/'))/kk/assets",
 "--cache_root=$run\cache","--log_file=$run\run.log",'--log_max_file_size_mb=500')
if ($Gtao) { $overrides+=@('--ao_mode=1','--ao_strength=2','--native_ao_method=gtao') }
$names=@($overrides | ForEach-Object { ($_ -split '=',2)[0] })
$gameArgs=@($sceneArgs | Where-Object { $names -notcontains (($_ -split '=',2)[0]) }) + $overrides
@{kind=$Kind;tables=$Tables;chapter=$Chapter;scene_run=$SceneRun;args=$gameArgs;
 dll_sha256=(Get-FileHash "$run\rexgpu-native.dll").Hash;limit_s=$limit;expected=$expected} | ConvertTo-Json -Depth 6 | Set-Content "$root\test.json"
if (!(Test-Path -LiteralPath $lockPath)) { Set-Content -LiteralPath $lockPath 'free' }
while ($true) {
 try { $state=(Get-Content -LiteralPath $lockPath -Raw).Trim() } catch {
  Write-Output 'Waiting: shared game lock is being updated.'
  Start-Sleep -Seconds 30
  continue
 }
 if ($state -match '^user') { Write-Output "USER LOCK: no launch. Prepared $root"; exit 3 }
 try { $priority=if (Test-Path $priorityPath) { (Get-Content $priorityPath -Raw).Trim() } else { '' } } catch { $priority='busy' }
 $busy=@(Get-Process king_kong -ErrorAction SilentlyContinue)
 if ($state -eq 'free' -and !$busy -and (!$priority -or $priority -match '^task098(?:\s|$)')) { break }
 Write-Output "Waiting: lock=$state priority=$priority game_pids=$($busy.Id -join ',')"
 Start-Sleep -Seconds $(if ($busy | Where-Object { $_.Path -notlike 'F:\KK-native-renderer\runs\*' -and $_.Path -notlike 'F:\KK-native-renderer\wt\*' }) {60} else {30})
}
if (!(Get-Process kkbg2 -ErrorAction SilentlyContinue)) { throw 'kkbg2 must be running before launch.' }
if ((Get-Content $lockPath -Raw).Trim() -ne 'free') { throw 'Game lock changed; retry.' }
if (Get-Process king_kong -ErrorAction SilentlyContinue) { throw 'Another game appeared; retry.' }
if ((Test-Path $priorityPath) -and (Get-Content $priorityPath -Raw).Trim() -notmatch '^task098(?:\s|$)') { throw 'Another task has priority; retry.' }
$launchHistory='F:\KK-native-renderer\runs\task098-launches.txt'
if ((Test-Path $launchHistory) -and @(Get-Content $launchHistory).Count -ge 3) { throw 'Task 098 has used all three game launches.' }
Set-Content -LiteralPath $lockPath "task098 $(Get-Date -Format o)"
$game=$null
try {
 Add-Content -LiteralPath $launchHistory $root
 $game=Start-Process -FilePath "$run\king_kong.exe" -ArgumentList $gameArgs -WorkingDirectory $run -WindowStyle Hidden -PassThru
 Write-Output "RUN $root PID $($game.Id)"
 $clock=[Diagnostics.Stopwatch]::StartNew()
 $expectedTables=switch ($Tables) {
  'full' {'2D 16384, 3D 256, cube 2048, array 256, samplers 1024, buffers 16384'}
  'small' {'2D 1024, 3D 128, cube 128, array 128, samplers 128, buffers 1024'}
  'minimum' {'2D 128, 3D 128, cube 128, array 128, samplers 128, buffers 256'}
 }
 while (!$game.HasExited -and $clock.Elapsed.TotalSeconds -lt $limit) {
  if (!$tablesVerified -and (Test-Path "$run\run.log")) {
   $entry=Select-String -LiteralPath "$run\run.log" -Pattern 'Vulkan descriptor capacities:' -SimpleMatch -ErrorAction SilentlyContinue | Select-Object -First 1
   if ($entry) {
    if (!$entry.Line.Contains($expectedTables)) {
     Write-Output "Requested table mode was not applied: $($entry.Line)"
     $tableMismatch=$true
     break
    }
    $tablesVerified=$true
    Write-Output "Verified $Tables tables: $expectedTables"
   }
  }
  $other=@(Get-Process king_kong -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne $game.Id })
  if ($other.Count) {
   Write-Output "Another game appeared (pid $($other.Id -join ',')); closing only task098 pid $($game.Id)."
   $interrupted=$true
   break
  }
  $missing=@($expected | Where-Object { !(Test-Path (Join-Path $run $_)) })
  $frameEnds=if ($Kind -eq 'golden') { @(Select-String -LiteralPath "$run\run.log" -Pattern 'Frame log: frame end' -SimpleMatch -ErrorAction SilentlyContinue).Count } else {0}
  if (!$missing -and ($Kind -ne 'golden' -or $frameEnds -ge $scene.frame_log_frames)) { break }
  Start-Sleep -Seconds 2
 }
} finally {
 if ($game -and !$game.HasExited) { $null=$game.CloseMainWindow(); if (!$game.WaitForExit(15000)) { Stop-Process -Id $game.Id; $game.WaitForExit() } }
 for ($attempt=0; $attempt -lt 3; ++$attempt) {
  try {
   $owner=(Get-Content -LiteralPath $lockPath -Raw).Trim()
   if ($owner -match '^task098(?:\s|$)') { Set-Content -LiteralPath $lockPath 'free' }
   break
  } catch { Start-Sleep -Seconds 1 }
 }
}
if ($Kind -eq 'golden') {
 & $python -I $compare collect --run $SceneRun --run-dir $run --out-root $root --scenes $SceneRun --plugin native --label "task098 $Tables" "--trigger=$($scene.env.REX_DEV_FRAME_LOG)"
 $collectCode=$LASTEXITCODE
 & $python -I $compare compare --golden-root F:\KK-native-renderer\golden --test-root $root --scenes $SceneRun --plugin native
 $compareCode=$LASTEXITCODE
}
Write-Output "EVIDENCE $root"
$missing=@($expected | Where-Object { !(Test-Path (Join-Path $run $_)) })
@{wall_seconds=$clock.Elapsed.TotalSeconds;missing=$missing;interrupted=[bool]$interrupted;tables_verified=[bool]$tablesVerified;table_mismatch=[bool]$tableMismatch;collect_exit=$collectCode;compare_exit=$compareCode} | ConvertTo-Json -Depth 4 | Set-Content "$root\result.json"
if (!$tablesVerified -or $tableMismatch) { exit 2 }
if ($interrupted) { exit 2 }
if ($missing.Count) { exit 2 }
if ($Kind -eq 'golden' -and ($collectCode -or $compareCode)) { exit 1 }
