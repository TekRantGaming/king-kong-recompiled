param([ValidateSet('vulkan','d3d12')][string]$Api='vulkan',
      [ValidateSet('off','dlss')][string]$Upscaler='off', [int]$Chapter=8,
      [string]$PadScript='0:SKIP/4,5:LT/120')
$ErrorActionPreference='Stop'
$workspace='F:\KK-native-renderer\wt\codex-debugmenu'
$lock='F:\KK-native-renderer\runs\codex-game.lock'
$priority='F:\KK-native-renderer\runs\codex-game.priority'
if((Get-Content -LiteralPath $lock -Raw).Trim().StartsWith('user')) { throw 'USER LOCK: code work only' }
$run="F:\KK-native-renderer\runs\task059-$(Get-Date -Format yyyyMMdd-HHmmss)-$Api-$Upscaler"
New-Item -ItemType Directory -Path "$run\cache\native-shaders" -Force | Out-Null
$build="$workspace\kk\out\build\kk-dev-vulkan"
Get-ChildItem -LiteralPath $build -File | Where-Object { $_.Name -eq 'king_kong.exe' -or $_.Extension -eq '.dll' } | Copy-Item -Destination $run
Copy-Item -LiteralPath "$build\glyphs" -Destination $run -Recurse
Copy-Item -LiteralPath 'F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc\bin\x64\dxcompiler.dll' -Destination $run
Copy-Item -Path 'F:\KK-native-renderer\runs\task026-20261013-211423\cache\native-shaders\kkshaders-*.pack*' -Destination "$run\cache\native-shaders"
robocopy 'F:\KK-native-renderer\userdata' "$run\userdata" /E /NFL /NDL /NJH /NJS /NP | Out-Null
if($LASTEXITCODE -ge 8) { throw 'Private user data copy failed' }
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
$env:KK_DEV_AUTOSKIP='1'; $env:KK_DEV_SCRIPT=$PadScript; $env:KK_DEV_EFFECT_TEST=$run
$env:KK_DEV_DLSS_LOG="$run\frames.csv"; $env:KK_DEV_DT_LOG='0,200'
$env:KK_DEV_DT_DIR=$run
$launch=@('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
 '--kk_renderer=native',"--native_backend=$Api", "--native_upscaler=$Upscaler",'--kk_frame_rate=30',
 "--kk_start=chapter:0:$Chapter",'--kk_launcher=false',"--user_data_root=$run\userdata",
 "--game_data_root=$workspace/kk/assets","--log_file=$run\run.log","--cache_root=$run\cache",
 "--config=$run\king_kong.toml",'--native_ao_method=gtao','--native_ao_apply=opaque',
 '--native_validation=true','--native_stutter_stats=true','--native_debug=32768',
 '--kk_check_updates=false','--kk_online_count=false','--kk_discord_status=false')
$game=$null
$renderGame=$null
$samples=[Collections.Generic.List[object]]::new()
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Task059Window {
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[Task059Window]::SetProcessDPIAware() | Out-Null
function Capture-OwnedWindow($process,$path) {
 $process.Refresh()
 $rect=New-Object Task059Window+RECT
 [Task059Window]::GetClientRect($process.MainWindowHandle,[ref]$rect) | Out-Null
 if($rect.R -le 0 -or $rect.B -le 0) { return }
 $bmp=New-Object System.Drawing.Bitmap $rect.R,$rect.B
 $graphics=[System.Drawing.Graphics]::FromImage($bmp)
 $dc=$graphics.GetHdc()
 $ok=[Task059Window]::PrintWindow($process.MainWindowHandle,$dc,3)
 $graphics.ReleaseHdc($dc); $graphics.Dispose()
 if($ok) { $bmp.Save($path,[System.Drawing.Imaging.ImageFormat]::Png) }
 $bmp.Dispose()
}
try {
 while(!$game) {
  if((Get-Content -LiteralPath $lock -Raw).Trim().StartsWith('user')) { throw 'USER LOCK: code work only' }
  if((Test-Path $priority) -and (Get-Content -LiteralPath $priority -Raw) -notmatch 'task059') {
   Write-Output 'Another task has priority; waiting 30 seconds'; Start-Sleep -Seconds 30; continue
  }
  $guard=$null
  try {
   $guard=[IO.File]::Open($lock,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::Read)
   $reader=[IO.StreamReader]::new($guard,[Text.Encoding]::UTF8,$true,1024,$true)
   $state=$reader.ReadToEnd().Trim(); $reader.Dispose()
   if($state.StartsWith('user')) { throw 'USER LOCK: code work only' }
   $others=@(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'")
   if($state -eq 'free' -and !$others.Count) {
    if(!(Get-Process kkbg2 -ErrorAction SilentlyContinue)) { throw 'kkbg2 required' }
    $bytes=[Text.Encoding]::ASCII.GetBytes("task059 $(Get-Date -Format o)`r`n")
    $guard.SetLength(0); $guard.Position=0; $guard.Write($bytes,0,$bytes.Length); $guard.Flush()
    $game=Start-Process -FilePath "$run\king_kong.exe" -ArgumentList $launch -WorkingDirectory $run -WindowStyle Hidden -PassThru
   }
  } catch [IO.IOException] { }
  finally { if($guard) { $guard.Dispose() } }
  if(!$game) { Write-Output "Game slot busy: $state"; Start-Sleep -Seconds 30 }
 }
 Write-Output "RUN $run PID $($game.Id)"
 $timer=[Diagnostics.Stopwatch]::StartNew()
 $done=$false
 $uiCaptured=$false
 while(!$game.HasExited -and $timer.Elapsed.TotalSeconds -lt 240) {
  Start-Sleep -Seconds 1
  $game.Refresh()
  # The safe-startup guard owns a renderer child. Track only a child of the process we started.
  $child=Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'" | Where-Object { $_.ParentProcessId -eq $game.Id -and $_.ExecutablePath -eq "$run\king_kong.exe" } | Select-Object -First 1
  if($child) { $renderGame=Get-Process -Id $child.ProcessId }
  if(!$renderGame) { continue }
  $renderGame.Refresh()
  $samples.Add([pscustomobject]@{seconds=$timer.Elapsed.TotalSeconds;private_bytes=$renderGame.PrivateMemorySize64;working_set=$renderGame.WorkingSet64})
  if(Test-Path "$run\run.log") {
   try {
    $stream=[IO.File]::Open("$run\run.log",[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    $reader=[IO.StreamReader]::new($stream)
    $log=$reader.ReadToEnd(); $reader.Dispose()
   } catch [IO.IOException] { continue }
   if(!$uiCaptured -and $log -match 'KK effect test: step 11 ') {
    Capture-OwnedWindow $renderGame "$run\overlay-open.png"
    [Task059Window]::PostMessage($renderGame.MainWindowHandle,0x100,[IntPtr]0x75,[IntPtr]0) | Out-Null
    [Task059Window]::PostMessage($renderGame.MainWindowHandle,0x101,[IntPtr]0x75,[IntPtr]0) | Out-Null
    Start-Sleep -Seconds 1
    Capture-OwnedWindow $renderGame "$run\overlay-f6-close.png"
    [Task059Window]::PostMessage($renderGame.MainWindowHandle,0x100,[IntPtr]0x75,[IntPtr]0) | Out-Null
    [Task059Window]::PostMessage($renderGame.MainWindowHandle,0x101,[IntPtr]0x75,[IntPtr]0) | Out-Null
    Start-Sleep -Seconds 1
    Capture-OwnedWindow $renderGame "$run\overlay-f6-open.png"
    $uiCaptured=$true
   }
   if($log -match 'KK effect test: done') {
    $done=$true
    for($cooldown=0; $cooldown -lt 10; ++$cooldown) {
     Start-Sleep -Seconds 1
     $renderGame.Refresh()
     $samples.Add([pscustomobject]@{seconds=$timer.Elapsed.TotalSeconds;private_bytes=$renderGame.PrivateMemorySize64;working_set=$renderGame.WorkingSet64})
    }
    break
   }
  }
 }
 $samples | Export-Csv -LiteralPath "$run\process-memory.csv" -NoTypeInformation
 Write-Output "Test sequence completed: $done"
} finally {
 if($game -and !$game.HasExited) {
  # The game's window close returns to the launcher by spawning a process. Stop only our PID.
  Stop-Process -Id $game.Id; $game.WaitForExit()
 }
 if($renderGame -and !$renderGame.HasExited) { Stop-Process -Id $renderGame.Id; $renderGame.WaitForExit() }
 if((Get-Content -LiteralPath $lock -Raw).Trim().StartsWith('task059 ')) { Set-Content -LiteralPath $lock -Value 'free' -Encoding ascii }
}
Write-Output "DONE $run"
