param([int]$Chapter=8, [ValidateSet('warp','vulkan')][string]$Backend='vulkan',
 [ValidateSet('off','dlss','fsr')][string]$Upscaler='off', [switch]$Gtao, [string]$Label='baseline')
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path "$PSScriptRoot/../..").Path
$lockPath='F:\KK-native-renderer\runs\codex-game.lock'
function Read-Lock {
 if (!(Test-Path -LiteralPath $lockPath)) { Set-Content -LiteralPath $lockPath 'free' }
 $state=(Get-Content -LiteralPath $lockPath -Raw).Trim()
 if ($state.StartsWith('user')) { throw 'USER LOCK: game runs prohibited; do code-only work.' }
 return $state
}
$null=Read-Lock
$run="F:\KK-native-renderer\runs\task035-$Label-$Backend-$Upscaler-ch$Chapter-$(Get-Date -Format yyyyMMdd-HHmmss)"
New-Item -ItemType Directory -Path $run | Out-Null
$build="$workspace\kk\out\build\kk-dev-vulkan"
Get-ChildItem -LiteralPath $build -File | Where-Object { $_.Extension -in '.exe','.dll' } | Copy-Item -Destination $run
Copy-Item -LiteralPath "$build\glyphs" -Destination $run -Recurse
Copy-Item -LiteralPath 'F:\KK-native-renderer\cache\native-shaders\kkshaders-spirv.pack' -Destination $run
Copy-Item -LiteralPath 'F:\KK-native-renderer\runs\task025-20261013\sanity-d3d12-ch8-210057\kkshaders-dxil.pack' -Destination $run
Copy-Item -LiteralPath 'F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc\bin\x64\dxcompiler.dll' -Destination $run
robocopy F:\KK-native-renderer\userdata "$run\userdata" /E /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'User data copy failed.' }
Get-ChildItem Env: | Where-Object { $_.Name -like 'KK_DEV_*' -or $_.Name -like 'REX_DEV_*' -or $_.Name -like 'VK_*' } | ForEach-Object { Remove-Item "Env:$($_.Name)" }
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'
$env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
$env:KK_DEV_AUTOSKIP='1'
$env:KK_DEV_SCRIPT='0:SKIP/120'
$env:KK_DEV_SHOTS='30,120'
$env:KK_DEV_SHOTS_FROM='menu'
$env:KK_DEV_SHOTS_DIR=$run
$env:KK_DEV_DT_LOG='0,240'
if ($Backend -eq 'vulkan') {
 @('khronos_validation.validate_sync = true','khronos_validation.gpuav_enable = true',
   'khronos_validation.report_flags = info,warn,error','khronos_validation.log_duplicate_limit = 20',
   'khronos_validation.debug_action = VK_DBG_LAYER_ACTION_LOG_MSG',
   "khronos_validation.log_filename = $run\vulkan-validation.log") | Set-Content "$run\vk_layer_settings.txt"
 $env:VK_LAYER_PATH='F:\KK-native-renderer\downloads\vulkan-sdk-latest\sdk\Bin'
 $env:VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation'
 $env:VK_LAYER_SETTINGS_PATH=$run
}
$api=if ($Backend -eq 'warp') {'d3d12'} else {'vulkan'}
$args=@('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
 "--kk_start=chapter:0:$Chapter",'--kk_launcher=false','--kk_cheats=true',
 "--user_data_root=$run\userdata","--game_data_root=$($workspace.Replace('\','/'))/kk/assets","--log_file=$run\run.log",
 "--cache_root=$run\cache",'--kk_original_look=true',
 '--kk_original_look_keep=kk_frame_rate,render_scale,native_render_scale,native_upscaler,ao_mode,ao_strength',
 '--kk_frame_rate=30','--native_debug=32768',"--native_backend=$api",'--native_render_scale=1280x720',
 "--native_upscaler=$Upscaler",'--native_upscaler_quality=quality','--native_upscaler_dlss_preset=latest',
 '--native_validation=true','--native_async_pipelines=false')
if ($Gtao) { $args+='--ao_mode=1','--ao_strength=2','--native_ao_method=gtao' } else { $args+='--ao_mode=0' }
if ($Backend -eq 'warp') { $args+='--d3d12_adapter=-2','--d3d12_queue_priority=0' }
@{chapter=$Chapter;backend=$Backend;upscaler=$Upscaler;gtao=[bool]$Gtao;args=$args;
 dll_sha256=(Get-FileHash "$run\rexgpu-native.dll").Hash;exe_sha256=(Get-FileHash "$run\king_kong.exe").Hash} | ConvertTo-Json | Set-Content "$run\test.json"
Get-ChildItem -LiteralPath $run -File | Where-Object { $_.Extension -in '.exe','.dll','.pack' } |
 ForEach-Object { [PSCustomObject]@{file=$_.Name;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash} } |
 ConvertTo-Json | Set-Content "$run\artifacts.json"
while ($true) {
 $state=Read-Lock
 $games=@(Get-CimInstance Win32_Process -Filter "name = 'king_kong.exe'")
 if ($state -eq 'free' -and $games.Count -eq 0) { break }
 $userGame=@($games | Where-Object { !$_.ExecutablePath -or ($_.ExecutablePath -notlike 'F:\KK-native-renderer\runs\*' -and $_.ExecutablePath -notlike 'F:\KK-native-renderer\wt\*') })
 Start-Sleep -Seconds $(if ($userGame.Count) {60} else {30})
}
if (!(Get-Process kkbg2 -ErrorAction SilentlyContinue)) { throw 'kkbg2 is required.' }
if ((Read-Lock) -ne 'free') { throw 'Lock changed before launch.' }
if (Get-CimInstance Win32_Process -Filter "name = 'king_kong.exe'") { throw 'A game started before launch; retry after it closes.' }
Set-Content -LiteralPath $lockPath "task035 $(Get-Date -Format o)"
$game=$null
try {
 $game=Start-Process -FilePath "$run\king_kong.exe" -ArgumentList $args -WorkingDirectory $run -WindowStyle Hidden -PassThru -RedirectStandardOutput "$run\stdout.log" -RedirectStandardError "$run\stderr.log"
 Write-Output "RUN $run PID $($game.Id)"
 $timer=[Diagnostics.Stopwatch]::StartNew()
 while (!$game.HasExited -and $timer.Elapsed.TotalSeconds -lt 240) {
  if (Test-Path "$run\shot_1200.bmp") { break }
  Start-Sleep -Seconds 2
 }
} finally {
 if ($game -and !$game.HasExited) { $null=$game.CloseMainWindow(); if (!$game.WaitForExit(15000)) { Stop-Process -Id $game.Id; $game.WaitForExit() } }
 @{seconds=$timer.Elapsed.TotalSeconds;exit_code=$game.ExitCode;capture120=(Test-Path "$run\shot_1200.bmp")} | ConvertTo-Json | Set-Content "$run\result.json"
 Set-Content -LiteralPath $lockPath 'free'
}
Write-Output "DONE $run"
