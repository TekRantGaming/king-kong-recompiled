param([string]$Label = 'before', [int[]]$Entries = @(2,3,4,5,6,7,8),
      [string]$Build = '', [string[]]$ExtraArgs = @(), [int]$Seconds = 60, [switch]$Hands)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
if (!$Build) { $Build = "$root\kk\out\build\kk-dev-vulkan" }
$lock = 'F:\KK-native-renderer\runs\codex-game.lock'
foreach ($entry in $Entries) {
  while ((Test-Path $lock) -and (Get-Content $lock -Raw).Trim() -ne 'free') {
    Write-Output "Waiting for shared game lock: $(Get-Content $lock -Raw)"
    Start-Sleep -Seconds 30
  }
  $run = "F:\KK-native-renderer\runs\task016-$Label-e$entry-$(Get-Date -Format yyyyMMdd-HHmmss)"
  New-Item -ItemType Directory -Path "$run\glyphs","$run\cache\native-shaders" -Force | Out-Null
  Get-ChildItem $Build -File | Where-Object { $_.Extension -in '.exe','.dll' } | Copy-Item -Destination $run
  Copy-Item "$root\kk\glyphs\*" "$run\glyphs" -Recurse
  Copy-Item 'F:\KK-native-renderer\cache\native-shaders\kkshaders-spirv.pack' "$run\cache\native-shaders"
  Copy-Item 'F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc\bin\x64\dxcompiler.dll' $run
  & robocopy 'F:\KK-native-renderer\userdata' "$run\userdata" /E /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) { throw 'Private user data copy failed' }
  $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = '0'
  $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED = '0'
  $env:KK_DEV_AUTOSKIP = '1'
  $env:KK_DEV_SHOTS_FROM = 'menu'
  $env:KK_DEV_SHOTS_DIR = $run
  $env:KK_DEV_SCRIPT = '0:SKIP/8,10:RSRIGHT/3,16:RSLEFT/6,24:RSRIGHT/3,30:RSDOWN/1,34:RSUP/1,45:A/0.1'
  if ($Hands) { $env:KK_DEV_SCRIPT = '0:SKIP/8,10:LB/0.2,12:LT/34,16:RSRIGHT/1,20:RSLEFT/1,30:RSDOWN/0.2,34:RSUP/0.2,45:A/0.1' }
  $env:KK_DEV_SHOTS = '9,12,15,18,21,24,27,30,33,36,39,39.1,39.2,39.3,39.4,42,45'
  $env:KK_DEV_SHOTS_FINE = if ($Hands) { '1' } else { '0' }
  if ($Hands) { $env:KK_DEV_SHOTS = '9,12,15,18,21,24,27,30,33,36,39,39.034,39.068,39.102,39.136,42,45' }
  $env:KK_DEV_FREEZE_SKIP = '3'
  $args = @('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
    '--kk_launcher=false',"--kk_start=chapter:0:$entry",'--kk_renderer=native','--native_backend=vulkan',
    '--native_render_scale=1','--kk_original_look_keep=ao_mode,ao_strength','--ao_mode=2','--ao_strength=3',
    '--native_ao_method=gtao','--native_ao_apply=opaque','--native_ao_dump_every=300','--native_ao_gpu_time=true',
    "--game_data_root=$root/kk/assets","--user_data_root=$run/userdata","--cache_root=$run/cache","--log_file=$run/run.log")
  $names = @($ExtraArgs | ForEach-Object { ($_ -split '=',2)[0] })
  $args = @($args | Where-Object { $names -notcontains ($_ -split '=',2)[0] }) + $ExtraArgs
  if ($Hands) { $args += @('--kk_cheats=true','--kk_cheat_shotgun=true','--kk_cheat_ammo=true','--kk_toggle_aim=false') }
  $proc = $null
  try {
    # Setup may take long enough for another task to claim the lock. Check again immediately before launch.
    while ((Test-Path $lock) -and (Get-Content $lock -Raw).Trim() -ne 'free') {
      Write-Output "Waiting for shared game lock after setup: $(Get-Content $lock -Raw)"
      Start-Sleep -Seconds 30
    }
    Set-Content $lock "task016 $(Get-Date -Format o)" -Encoding ascii
    $proc = Start-Process "$run\king_kong.exe" -WorkingDirectory $run -ArgumentList $args -WindowStyle Hidden -PassThru
    Write-Output "Started $run PID $($proc.Id)"
    $end = (Get-Date).AddSeconds($Seconds)
    while (!$proc.HasExited -and (Get-Date) -lt $end) { Start-Sleep -Seconds 1 }
  } finally {
    if ($proc -and !$proc.HasExited) {
      [void]$proc.CloseMainWindow()
      if (!$proc.WaitForExit(10000)) { Stop-Process -Id $proc.Id -Force; $proc.WaitForExit() }
    }
    Set-Content $lock 'free' -Encoding ascii
  }
  Write-Output "Finished $run"
}
