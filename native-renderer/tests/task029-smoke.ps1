# TASK-029: exactly one short smoke launch. No desktop captures, no other processes stopped.
param([string]$Build = '', [int]$Seconds = 60)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'run-storage.ps1')
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
if (!$Build) { $Build = Join-Path $repo 'kk\out\build\kk-dev-vulkan' }
$lockPath = 'F:\KK-native-renderer\runs\codex-game.lock'
$run = 'F:\KK-native-renderer\runs\task029-smoke-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
if (!(Test-Path $lockPath)) { Set-Content $lockPath 'free' -Encoding ascii }
function Check-UserLock {
  if ((Get-Content -LiteralPath $lockPath -Raw).Trim() -match '^user') { throw 'User lock: smoke launch forbidden, do not wait' }
}
Check-UserLock
if (!(Get-Process kkbg2 -ErrorAction SilentlyContinue)) { throw 'kkbg2 must already be running and will be left running' }
Initialize-RunBuild -Build $Build -Run $run -ShaderPacks @('F:\KK-native-renderer\cache\native-shaders\kkshaders-spirv.pack') -Dxc 'F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc\bin\x64\dxcompiler.dll'
& robocopy 'F:\KK-native-renderer\userdata' "$run\userdata" /E /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'Private userdata copy failed' }
$guard = $null; $proc = $null
try {
  while (!$guard) {
    Check-UserLock
    $candidate = $null
    try {
      $candidate = [IO.File]::Open($lockPath, 'Open', 'ReadWrite', 'Read')
      $reader = New-Object IO.StreamReader($candidate, [Text.Encoding]::ASCII, $false, 1024, $true)
      $state = $reader.ReadToEnd().Trim(); $reader.Dispose()
      if ($state -match '^user') { throw 'User lock: smoke launch forbidden' }
      $ours = @(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'" | Where-Object {
        $_.ExecutablePath -like 'F:\KK-native-renderer\runs\*' -or $_.ExecutablePath -like 'F:\KK-native-renderer\wt\*'
      })
      if ($state -eq 'free' -and !$ours.Count) { $guard = $candidate; $candidate = $null }
    } catch [IO.IOException] { Write-Output 'Lock is being held by another task' }
    finally { if ($candidate) { $candidate.Dispose() } }
    if (!$guard) { Write-Output 'Waiting for project game lock, checking in 30 seconds'; Start-Sleep -Seconds 30 }
  }
  $bytes = [Text.Encoding]::ASCII.GetBytes("task029 $(Get-Date -Format o)`r`n")
  $guard.SetLength(0); $guard.Position=0; $guard.Write($bytes,0,$bytes.Length); $guard.Flush()
  Get-ChildItem Env: | Where-Object { $_.Name -like 'KK_*' -or $_.Name -like 'REX_DEV_*' } | ForEach-Object { Remove-Item -LiteralPath ('Env:' + $_.Name) }
  $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
  $env:KK_DEV_AUTOSKIP='1'; $env:KK_DEV_SHOTS_FROM='menu'; $env:KK_DEV_SHOTS_DIR=$run
  $env:KK_DEV_SHOTS='4,4.1,4.2,6,8'; $env:KK_DEV_SCRIPT='0:SKIP/3'
  $args = @('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
    '--kk_launcher=false','--kk_start=chapter:0:8','--kk_renderer=native','--native_backend=vulkan',
    '--native_render_scale=1','--native_upscaler=off','--ao_mode=0','--kk_frame_rate=30',
    "--game_data_root=$($repo.Replace('\','/'))/kk/assets","--user_data_root=$run/userdata",
    "--cache_root=$run/cache","--log_file=$run/run.log")
  $args | Set-Content "$run\arguments.txt"
  $started = Get-Date
  $proc = Start-Process "$run\king_kong.exe" -WorkingDirectory $run -ArgumentList $args -WindowStyle Hidden -PassThru
  $proc.Id | Set-Content "$run\pid.txt"
  Write-Output "Smoke PID $($proc.Id), run $run"
  while (!$proc.HasExited -and ((Get-Date)-$started).TotalSeconds -lt $Seconds) {
    if (Test-Path "$run\shot_80.png") { break }
    Start-Sleep -Seconds 1
  }
} finally {
  if ($proc -and !$proc.HasExited) {
    [void]$proc.CloseMainWindow()
    if (!$proc.WaitForExit(10000)) { Stop-Process -Id $proc.Id -Force; $proc.WaitForExit() }
  }
  if ($guard) {
    $bytes = [Text.Encoding]::ASCII.GetBytes("free`r`n")
    $guard.SetLength(0); $guard.Position=0; $guard.Write($bytes,0,$bytes.Length); $guard.Flush(); $guard.Dispose()
  }
}
Assert-RunBuildUnchanged $run
Complete-RunCaptures $run
$shots = @(Get-ChildItem -LiteralPath $run -Filter 'shot_*.png')
if ($shots.Count -ne 5 -or (Get-ChildItem $run -Filter 'shot_*.bmp')) { throw 'Expected five PNG captures and no BMP captures' }
$manifest = Get-Content "$run\build-staging.json" -Raw | ConvertFrom-Json
$files = @(Get-ChildItem -LiteralPath $run -Recurse -File)
$sharedNames = @($manifest.files | Where-Object mode -eq 'hardlink' | ForEach-Object name)
$private = @($files | Where-Object { $_.DirectoryName -ne $run -or $sharedNames -notcontains $_.Name })
$privateBytes = [long](($private | Measure-Object Length -Sum).Sum)
$pngBytes = [long](($shots | Measure-Object Length -Sum).Sum)
Add-Type -AssemblyName System.Drawing
$bmpBytes=0L
foreach ($shot in $shots) {
  $signature = [IO.File]::ReadAllBytes($shot.FullName)[0..7]
  if ([BitConverter]::ToString($signature) -ne '89-50-4E-47-0D-0A-1A-0A') { throw 'Capture has no PNG signature' }
  $image = [Drawing.Image]::FromFile($shot.FullName)
  try { $bmpBytes += 54L + 4L * $image.Width * $image.Height } finally { $image.Dispose() }
}
$beforeBytes = $privateBytes - $pngBytes + $bmpBytes + [long]$manifest.linked_bytes
[pscustomobject]@{run=$run; pid=$proc.Id; seconds=((Get-Date)-$started).TotalSeconds;
  captures=$shots.Count; png_bytes=$pngBytes; equivalent_bmp_bytes=$bmpBytes;
  shared_artifact_bytes=$manifest.linked_bytes; copied_artifact_bytes=$manifest.copied_bytes;
  before_bytes_same_workload=$beforeBytes; after_incremental_bytes=$privateBytes;
  logical_run_bytes=[long](($files | Measure-Object Length -Sum).Sum);
  staging_hash=$manifest.build_hash; unchanged_shared_files=$true; kkbg2_running=[bool](Get-Process kkbg2 -ErrorAction SilentlyContinue)} |
  ConvertTo-Json | Set-Content "$run\disk-results.json" -Encoding UTF8
Get-Content "$run\disk-results.json"
