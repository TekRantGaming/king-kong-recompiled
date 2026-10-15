param([string]$DxilPack='F:\KK-native-renderer\runs\task026-20261013-211423\cache\native-shaders\kkshaders-dxil.pack',
  [string]$ResumeRun='')
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path "$PSScriptRoot/../..").Path
$lockPath='F:\KK-native-renderer\runs\codex-game.lock'
$priorityPath='F:\KK-native-renderer\runs\codex-game.priority'
$taskName='task074'
$run="F:\KK-native-renderer\runs\task074-$(Get-Date -Format yyyyMMdd-HHmmss)"
$phases=@('launcher','vulkan','d3d12','legacy')
if ($ResumeRun) {
  $run=(Resolve-Path $ResumeRun).Path
  if (!$run.StartsWith('F:\KK-native-renderer\runs\task074-')) { throw 'Resume path outside task074 runs' }
  if (!(Test-Path "$run\launcher\graphics-bottom.png") -or !(Test-Path "$run\vulkan\shot_50.png")) { throw 'Completed phases missing' }
  if (!(Select-String -Path "$run\vulkan\run.log" -SimpleMatch -Pattern 'startup finished (first game frame shown)')) { throw 'Vulkan checkpoint missing' }
  $phases=@('d3d12','legacy')
} else { New-Item -ItemType Directory -Path $run | Out-Null }
$build="$workspace\kk\out\build\kk-dev-vulkan"
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Task074Window {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[Task074Window]::SetProcessDPIAware() | Out-Null
function Save-OwnWindow($process,$path) {
  $process.Refresh()
  $rect=New-Object Task074Window+RECT
  if (!$process.MainWindowHandle -or ![Task074Window]::GetClientRect($process.MainWindowHandle,[ref]$rect)) { throw 'Own window unavailable' }
  $bmp=New-Object System.Drawing.Bitmap ($rect.R-$rect.L),($rect.B-$rect.T)
  $graphics=[Drawing.Graphics]::FromImage($bmp); $dc=$graphics.GetHdc()
  $ok=[Task074Window]::PrintWindow($process.MainWindowHandle,$dc,3)
  $graphics.ReleaseHdc($dc); $graphics.Dispose()
  if ($ok) { $bmp.Save($path,[Drawing.Imaging.ImageFormat]::Png) }
  $bmp.Dispose()
  if (!$ok) { throw 'PrintWindow failed' }
}
function Claim-Lock {
  if (!(Test-Path $lockPath)) { Set-Content $lockPath 'free' }
  while ($true) {
    $owner=(Get-Content $lockPath -Raw).Trim()
    if ($owner.StartsWith('user')) { Write-Output "SKIPPED: user lock; $run"; exit 3 }
    $games=@(Get-Process king_kong -ErrorAction SilentlyContinue)
    $priorityOk=!(Test-Path $priorityPath) -or (Get-Content $priorityPath -Raw).StartsWith($taskName)
    if ($owner -eq 'free' -and !$games.Count -and $priorityOk) {
      $file=$null
      try {
        $file=[IO.File]::Open($lockPath,'Open','ReadWrite','None')
        $reader=[IO.StreamReader]::new($file,[Text.Encoding]::UTF8,$true,1024,$true)
        $current=$reader.ReadToEnd().Trim(); $reader.Dispose()
        if ($current -eq 'free') {
          $file.Position=0; $file.SetLength(0)
          $bytes=[Text.Encoding]::UTF8.GetBytes("$taskName $(Get-Date -Format o)")
          $file.Write($bytes,0,$bytes.Length); $file.Flush(); $file.Dispose(); return
        }
      } finally { if ($file) { $file.Dispose() } }
    }
    Write-Output "WAIT: $owner"
    Start-Sleep -Seconds 30
  }
}
# Every phase has a fresh package and private user data; no files are deleted.
$results=@()
if ($ResumeRun) { $results=@(Get-Content "$run\results.json" -Raw | ConvertFrom-Json) }
foreach ($phase in $phases) {
  $dir=New-Item -ItemType Directory -Path "$run\$phase"
  Get-ChildItem $build -File | Where-Object { $_.Extension -eq '.dll' } | Copy-Item -Destination $dir
  Copy-Item "$build\king_kong.exe" $dir
  Copy-Item "$build\glyphs" $dir -Recurse
  Copy-Item 'F:\KK-native-renderer\cache\native-shaders\kkshaders-spirv.pack' $dir
  Copy-Item $DxilPack "$dir\kkshaders-dxil.pack"
  robocopy 'F:\KK-native-renderer\userdata' "$dir\userdata" /E /NFL /NDL /NJH /NJS | Out-Null
  if ($LASTEXITCODE -ge 8) { throw 'Private data copy failed' }
  if (Get-ChildItem $dir -Filter '*xenos*') { throw 'Xenos file staged' }
  Get-ChildItem $dir -File | Select-Object Name,Length | ConvertTo-Json | Set-Content "$dir\package-files.json"
  Get-ChildItem Env: | Where-Object { $_.Name -like 'KK_DEV_*' -or $_.Name -like 'REX_DEV_*' -or $_.Name -eq 'KK_STARTUP_PIPE' } | ForEach-Object { Remove-Item "Env:$($_.Name)" }
  $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
  $env:KK_TEST_STARTUP_LOCK=$lockPath; $env:KK_TEST_STARTUP_OWNER=$taskName
  $env:KK_DEV_AUTOSKIP='1'; $env:KK_DEV_SCRIPT='0:SKIP/20'
  $env:KK_DEV_SHOTS='5'; $env:KK_DEV_SHOTS_FROM='menu'; $env:KK_DEV_SHOTS_DIR="$dir"
  $env:KK_AUTOPLAY='100000'
  $launcher=$phase -eq 'launcher'
  if ($launcher) {
    # An existing-version config avoids the setup shader screen covering the Graphics tour.
    @('kk_last_version = "2.0.0-beta.4"','kk_settings_version = 2') | Set-Content "$dir\king_kong.toml"
    $env:KK_DEV_LAUNCHER_TOUR='4'
    $env:KK_DEV_LAUNCHER_INPUT='shot:wait,shot:wait,shot:wait,shot:wait,move:700:500,wheel:-12'
  }
  if ($phase -eq 'legacy') {
    @('kk_renderer = "classic"','gpu_plugin = "xenos"','native_backend = "d3d12"','kk_settings_version = 2') | Set-Content "$dir\king_kong.toml"
    New-Item -ItemType Directory -Force "$dir\userdata\renderer-recovery" | Out-Null
    @('d3d12','classic','1','old fallback','1','1') | Set-Content "$dir\userdata\renderer-recovery\fallback.state"
    Set-Content "$dir\userdata\renderer-recovery\startup.marker" 'd3d12'
  }
  $api=if ($phase -eq 'd3d12') { 'd3d12' } else { 'vulkan' }
  $args=@('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
    "--kk_launcher=$($launcher.ToString().ToLower())",'--kk_start=chapter:0:8',
    "--user_data_root=$dir\userdata","--game_data_root=$($workspace.Replace('\','/'))/kk/assets",
    "--log_file=$dir\run.log","--cache_root=$dir\cache",'--kk_original_look=true',
    '--kk_frame_rate=30','--native_upscaler=off','--native_render_scale=1280x720','--ao_mode=0',
    '--kk_check_updates=false','--kk_online_count=false','--kk_discord_status=false','--native_launcher_prewarm=false')
  if ($phase -ne 'legacy') { $args+="--native_backend=$api" }
  Claim-Lock
  $parent=$null; $known=@{}; $passed=$false
  try {
    if (!(Get-Process kkbg2 -ErrorAction SilentlyContinue)) { throw 'kkbg2 is required' }
    if (Get-Process king_kong -ErrorAction SilentlyContinue) { throw 'Another game appeared' }
    if (!(Get-Content $lockPath -Raw).StartsWith("$taskName ")) { throw 'Lock changed' }
    $parent=Start-Process "$dir\king_kong.exe" -ArgumentList $args -WorkingDirectory $dir -WindowStyle Hidden -PassThru
    $known[$parent.Id]=$true
    Write-Output "RUN $phase $dir PID=$($parent.Id)"
    $clock=[Diagnostics.Stopwatch]::StartNew(); $graphicsAt=-1; $top=$false
    while (!$parent.HasExited -and $clock.Elapsed.TotalSeconds -lt 150) {
      if ((Get-Content $lockPath -Raw).StartsWith('user')) { throw 'User took the lock' }
      $children=@(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'" | Where-Object { $_.ExecutablePath -eq "$dir\king_kong.exe" })
      foreach ($child in $children) { if ($known.ContainsKey([int]$child.ParentProcessId)) { $known[[int]$child.ProcessId]=$true } }
      if ($launcher -and (Test-Path "$dir\run.log")) {
        $text=Get-Content "$dir\run.log" -Raw
        if ($text.Contains('KK dev: tour graphics')) {
          if ($graphicsAt -lt 0) { $graphicsAt=$clock.Elapsed.TotalSeconds }
          $window=$children | Where-Object { $known.ContainsKey([int]$_.ProcessId) -and $_.ProcessId -ne $parent.Id } | Select-Object -First 1
          if ($window) {
            $proc=Get-Process -Id $window.ProcessId
            if (!$top -and $clock.Elapsed.TotalSeconds -gt $graphicsAt+0.3) { Save-OwnWindow $proc "$dir\graphics-top.png"; $top=$true }
            if ($top -and $text.Contains('KK dev: input wheel:-12')) { Start-Sleep -Milliseconds 400; Save-OwnWindow $proc "$dir\graphics-bottom.png"; $passed=$true; break }
          }
        }
      } elseif (Test-Path "$dir\shot_50.png") { $passed=$true; break }
      Start-Sleep -Milliseconds 300
    }
    if (!$passed) { throw "Phase $phase did not reach its checkpoint" }
    if (!$launcher -and !(Select-String -Path "$dir\run.log" -SimpleMatch -Pattern 'startup finished (first game frame shown)')) { throw 'No first presented frame' }
    $results+=@{phase=$phase;passed=$passed;seconds=$clock.Elapsed.TotalSeconds;directory="$dir";pids=@($known.Keys)}
  } finally {
    if ($parent -and !$parent.HasExited) { Stop-Process -Id $parent.Id; $parent.WaitForExit() }
    # Only recorded descendants from this phase's private executable may be closed.
    foreach ($processId in @($known.Keys)) {
      if ($parent -and $processId -eq $parent.Id) { continue }
      $proc=Get-Process -Id $processId -ErrorAction SilentlyContinue
      if ($proc -and $proc.Path -eq "$dir\king_kong.exe") { $null=$proc.CloseMainWindow(); if (!$proc.WaitForExit(10000)) { Stop-Process -Id $processId; $proc.WaitForExit() } }
    }
    if ((Get-Content $lockPath -Raw).StartsWith("$taskName ")) { Set-Content $lockPath 'free' }
    $results | ConvertTo-Json -Depth 4 | Set-Content "$run\results.json"
  }
}
Write-Output "Chapter checkpoints passed; inspect launcher captures manually: $run"
