param([string]$Label='init-all', [ValidateSet('','init','crash','device-lost')][string]$Failure='init',
  [ValidateSet('vulkan','d3d12')][string]$Preferred='vulkan',
  [ValidateSet('all','vulkan','d3d12')][string]$FailApi='all',
  [string]$ReuseData='', [switch]$Launcher, [string]$LauncherInput='',
  [string]$DxilPack='F:\KK-native-renderer\runs\task025-20261013\sanity-d3d12-ch8-210057\kkshaders-dxil.pack')
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$lockPath='F:\KK-native-renderer\runs\codex-game.lock'
$run="F:\KK-native-renderer\runs\task039-$Label-$(Get-Date -Format yyyyMMdd-HHmmss)"
New-Item -ItemType Directory -Path $run | Out-Null
$build=Join-Path $workspace 'kk/out/build/kk-dev-vulkan'
Get-ChildItem -LiteralPath $build -File | Where-Object { $_.Extension -in '.exe','.dll' } | Copy-Item -Destination $run
Copy-Item -LiteralPath "$build\glyphs" -Destination $run -Recurse
Copy-Item -LiteralPath 'F:\KK-native-renderer\cache\native-shaders\kkshaders-spirv.pack' -Destination $run
Copy-Item -LiteralPath $DxilPack -Destination "$run\kkshaders-dxil.pack"
Copy-Item -LiteralPath 'F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc\bin\x64\dxcompiler.dll' -Destination $run
$data=if ($ReuseData) { $ReuseData } else { 'F:\KK-native-renderer\userdata' }
robocopy $data "$run\userdata" /E /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'Private user data copy failed.' }
# Hash the shared test data, not the user's Documents or Desktop copies.
$sourceHashes=@(Get-ChildItem 'F:\KK-native-renderer\userdata' -Recurse -File | ForEach-Object {
  @{path=$_.FullName;hash=(Get-FileHash -LiteralPath $_.FullName).Hash}
})
$sourceHashes | ConvertTo-Json -Depth 4 | Set-Content "$run\source-before.json"
Get-ChildItem Env: | Where-Object { $_.Name -like 'KK_DEV_*' -or $_.Name -like 'REX_DEV_*' -or $_.Name -eq 'KK_STARTUP_PIPE' } | ForEach-Object { Remove-Item "Env:$($_.Name)" }
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'
$env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
$env:KK_DEV_AUTOSKIP='1'
$env:KK_DEV_SCRIPT='0:SKIP/120'
$env:KK_DEV_SHOTS='120'
$env:KK_DEV_SHOTS_FROM='menu'
$env:KK_DEV_SHOTS_DIR=$run
# Suppress the launcher's network update/pack checks without autoplay during this short test.
$env:KK_AUTOPLAY='100000'
$env:KK_TEST_STARTUP_LOCK=$lockPath
if ($LauncherInput) { $env:KK_DEV_LAUNCHER_INPUT=$LauncherInput }
if ($Launcher) {
  Add-Type -AssemblyName System.Drawing
  Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Task039Window {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
  [Task039Window]::SetProcessDPIAware() | Out-Null
}
function Save-OwnWindow($p,$name) {
  $p.Refresh()
  if (!$p.MainWindowHandle) { return }
  $rect=New-Object Task039Window+RECT
  if (![Task039Window]::GetClientRect($p.MainWindowHandle,[ref]$rect)) { return }
  $bmp=New-Object System.Drawing.Bitmap ($rect.R-$rect.L),($rect.B-$rect.T)
  $graphics=[System.Drawing.Graphics]::FromImage($bmp)
  $dc=$graphics.GetHdc()
  $ok=[Task039Window]::PrintWindow($p.MainWindowHandle,$dc,3)
  $graphics.ReleaseHdc($dc); $graphics.Dispose()
  if ($ok) { $bmp.Save("$run\$name.png",[System.Drawing.Imaging.ImageFormat]::Png) }
  $bmp.Dispose()
}
$args=@('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
  '--kk_start=chapter:0:8',"--kk_launcher=$($Launcher.IsPresent.ToString().ToLower())",'--kk_cheats=true',
  "--user_data_root=$run\userdata","--game_data_root=$($workspace.Replace('\','/'))/kk/assets",
  "--log_file=$run\run.log","--cache_root=$run\cache",
  '--kk_original_look=true','--kk_original_look_keep=kk_frame_rate,native_render_scale,native_upscaler,ao_mode',
  '--kk_frame_rate=30','--native_render_scale=1280x720','--native_upscaler=off','--ao_mode=0',
  '--kk_online_count=false','--kk_discord_status=false','--kk_check_updates=false','--native_launcher_prewarm=false',
  "--native_backend=$Preferred","--kk_dev_startup_failure=$Failure","--kk_dev_startup_api=$FailApi")
@{label=$Label;failure=$Failure;preferred=$Preferred;fail_api=$FailApi;reuse_data=$ReuseData;args=$args;
  dxil_pack_source=$DxilPack;dxil_sha256=(Get-FileHash "$run\kkshaders-dxil.pack").Hash;
  exe_sha256=(Get-FileHash "$run\king_kong.exe").Hash;dll_sha256=(Get-FileHash "$run\rexgpu-native.dll").Hash} |
  ConvertTo-Json -Depth 4 | Set-Content "$run\test.json"
if (!(Test-Path -LiteralPath $lockPath)) { Set-Content -LiteralPath $lockPath 'free' }
while ($true) {
  $lock=(Get-Content -LiteralPath $lockPath -Raw).Trim()
  if ($lock.StartsWith('user')) { Write-Output "SKIPPED user lock: $run"; exit 3 }
  $ours=Get-Process king_kong -ErrorAction SilentlyContinue | Where-Object { $_.Path -like 'F:\KK-native-renderer\runs\*' -or $_.Path -like 'F:\KK-native-renderer\wt\*' }
  if ($lock -eq 'free' -and !$ours) {
    # Read and claim atomically with respect to cooperating writers.
    try {
      $file=[IO.File]::Open($lockPath,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
      $reader=[IO.StreamReader]::new($file,[Text.Encoding]::UTF8,$true,1024,$true)
      $current=$reader.ReadToEnd().Trim(); $reader.Dispose()
      if ($current -eq 'free') {
        $file.Position=0; $file.SetLength(0)
        $bytes=[Text.Encoding]::UTF8.GetBytes("task039 $(Get-Date -Format o)")
        $file.Write($bytes,0,$bytes.Length); $file.Flush(); $file.Dispose(); break
      }
      $file.Dispose()
    } catch { if ($file) { $file.Dispose() } }
  }
  Write-Output "WAIT $lock"
  Start-Sleep -Seconds 30
}
$parent=$null
$known=@{}
$outcome='timeout'
try {
  if (!(Get-Process kkbg2 -ErrorAction SilentlyContinue)) { throw 'kkbg2 must be running.' }
  if (!(Get-Content -LiteralPath $lockPath -Raw).Trim().StartsWith('task039 ')) { throw 'The test lock changed before launch.' }
  $parent=Start-Process -FilePath "$run\king_kong.exe" -ArgumentList $args -WorkingDirectory $run -WindowStyle Hidden -PassThru
  $known[$parent.Id]=$true
  Write-Output "RUN $run monitor PID $($parent.Id)"
  $clock=[Diagnostics.Stopwatch]::StartNew()
  $captured=@{}
  while (!$parent.HasExited -and $clock.Elapsed.TotalSeconds -lt 240) {
    if ((Get-Content -LiteralPath $lockPath -Raw).Trim().StartsWith('user')) { $outcome='user-pause'; break }
    $children=Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'" | Where-Object { $_.ExecutablePath -eq "$run\king_kong.exe" }
    foreach ($child in $children) {
      if ($known.ContainsKey([int]$child.ParentProcessId)) { $known[[int]$child.ProcessId]=$true }
    }
    if ($Launcher) {
      foreach ($time in @(2,10)) {
        if ($clock.Elapsed.TotalSeconds -ge $time -and !$captured.ContainsKey($time)) {
          foreach ($id in @($known.Keys)) {
            if ($id -eq $parent.Id) { continue }
            $p=Get-Process -Id $id -ErrorAction SilentlyContinue
            if ($p -and $p.Path -eq "$run\king_kong.exe" -and $p.MainWindowHandle) {
              Save-OwnWindow $p "launcher-$($time)s"; $captured[$time]=$true; break
            }
          }
        }
      }
    }
    if ($Launcher -and $clock.Elapsed.TotalSeconds -ge 120) { $outcome='launcher-120s'; break }
    if (Test-Path -LiteralPath "$run\shot_1200.bmp") { $outcome='chapter-120s'; break }
    Start-Sleep -Seconds 2
  }
  if ($parent.HasExited) { $outcome="exited-$($parent.ExitCode)" }
} finally {
  # Stop the monitor first. Closing a stalled child must not look like a crash that it should retry.
  if ($parent -and !$parent.HasExited) { Stop-Process -Id $parent.Id; $parent.WaitForExit() }
  # Capture children created since the last polling snapshot, and any normal launcher relaunch descendants.
  $remaining=@(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'" | Where-Object { $_.ExecutablePath -eq "$run\king_kong.exe" })
  do {
    $added=$false
    foreach ($child in $remaining) {
      if ($known.ContainsKey([int]$child.ParentProcessId) -and !$known.ContainsKey([int]$child.ProcessId)) {
        $known[[int]$child.ProcessId]=$true; $added=$true
      }
    }
  } while ($added)
  foreach ($child in $remaining) {
    if ($known.ContainsKey([int]$child.ProcessId) -and $child.CommandLine -notlike '*--kk_startup_child=true*') {
      Stop-Process -Id $child.ProcessId -ErrorAction SilentlyContinue
    }
  }
  # The monitor has no window; close only its recorded children, then its own PID if necessary.
  foreach ($id in @($known.Keys)) {
    if ($parent -and $id -eq $parent.Id) { continue }
    $p=Get-Process -Id $id -ErrorAction SilentlyContinue
    if ($p -and $p.Path -eq "$run\king_kong.exe") {
      $null=$p.CloseMainWindow()
      if (!$p.WaitForExit(10000)) { Stop-Process -Id $id; $p.WaitForExit() }
    }
  }
  if ((Get-Content -LiteralPath $lockPath -Raw).Trim().StartsWith('task039 ')) {
    Set-Content -LiteralPath $lockPath 'free'
  }
}
$unchanged=$true
foreach ($item in $sourceHashes) {
  if (!(Test-Path -LiteralPath $item.path) -or (Get-FileHash -LiteralPath $item.path).Hash -ne $item.hash) { $unchanged=$false }
}
@{outcome=$outcome;source_data_unchanged=$unchanged;pids=@($known.Keys);seconds=$clock.Elapsed.TotalSeconds;
  state=[string[]]@(Get-Content "$run\userdata\renderer-recovery\fallback.state" -ErrorAction SilentlyContinue);
  marker=[string](Get-Content "$run\userdata\renderer-recovery\startup.marker" -Raw -ErrorAction SilentlyContinue);
  marker_bytes=(Get-Item "$run\userdata\renderer-recovery\startup.marker" -ErrorAction SilentlyContinue).Length} |
  ConvertTo-Json -Depth 4 | Set-Content "$run\result.json"
Write-Output "DONE $outcome source-data-unchanged=$unchanged $run"
if ($outcome -eq 'user-pause' -or $outcome -eq 'exited-3') { exit 3 }
if ($outcome -notin @('chapter-120s','launcher-120s') -or !$unchanged) { exit 1 }
exit 0
