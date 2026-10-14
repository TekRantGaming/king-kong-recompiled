param([string]$Case='trace', [string]$SourceUser='F:\KK-native-renderer\userdata', [int]$Seconds=40,
      [string]$InputSteps='', [string]$PauseActions='', [string]$Shots='10,20,30', [int]$Chapter=8,
      [string]$SaveAt='', [string]$PadScript='0:SKIP/120')
$ErrorActionPreference='Stop'
$workspace='F:\KK-native-renderer\wt\codex-audio'
$lock='F:\KK-native-renderer\runs\codex-game.lock'
$state=(Get-Content -LiteralPath $lock -Raw).Trim()
if($state.StartsWith('user')) { throw 'USER LOCK: code work only' }
$run="F:\KK-native-renderer\runs\task032-$(Get-Date -Format yyyyMMdd-HHmmss)-$Case"
New-Item -ItemType Directory -Path $run | Out-Null
$build="$workspace\kk\out\build\kk-dev-vulkan"
Get-ChildItem -LiteralPath $build -File | Where-Object { $_.Name -eq 'king_kong.exe' -or $_.Extension -eq '.dll' } | Copy-Item -Destination $run
Copy-Item -LiteralPath "$build\glyphs" -Destination $run -Recurse
Copy-Item -LiteralPath 'F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc\bin\x64\dxcompiler.dll' -Destination $run
New-Item -ItemType Directory -Path "$run\cache\native-shaders" | Out-Null
Copy-Item -Path 'F:\KK-native-renderer\runs\task026-20261013-211423\cache\native-shaders\kkshaders-spirv.pack*' -Destination "$run\cache\native-shaders"
robocopy $SourceUser "$run\userdata" /E /NFL /NDL /NJH /NJS /NP | Out-Null
if($LASTEXITCODE -ge 8) { throw 'Private user data copy failed' }
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
$env:KK_DEV_AUTOSKIP='1'; $env:KK_DEV_SCRIPT=$PadScript
$env:KK_DEV_AUDIO_TRACE='1'; $env:KK_DEV_SHOTS_DIR=$run
$env:KK_DEV_SHOTS=$Shots; $env:KK_DEV_SHOTS_FROM='menu'
$env:KK_DEV_PAUSE_INPUT=$null; $env:KK_DEV_LAUNCHER_INPUT=$null
$env:KK_DEV_PAUSE_ACTIONS=$PauseActions
$env:KK_DEV_PAGE_TRACE='1'
$env:KK_DEV_SAVE_TRACE='1'
$env:KK_DEV_AUDIO_SAVE_AT=$SaveAt
$env:KK_DEV_JUMP_DUMPG='5'
$launch=@('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
 '--kk_renderer=native','--native_backend=vulkan','--native_upscaler=off','--kk_frame_rate=30',
 "--kk_start=chapter:0:$Chapter",'--kk_launcher=false',"--user_data_root=$run\userdata",
 "--game_data_root=$workspace/kk/assets","--log_file=$run\run.log","--cache_root=$run\cache",
 '--kk_check_updates=false','--kk_online_count=false','--kk_discord_status=false')
if($Case -eq 'launcher') {
 $launch=@($launch | Where-Object { $_ -notmatch '^--kk_start=|^--kk_launcher=' })
 $launch+=@('--kk_launcher=true','--kk_last_version=2.0.0-beta.3','--native_launcher_prewarm=false')
 $env:KK_DEV_LAUNCHER_INPUT=$InputSteps
 $env:KK_DEV_SHOTS=$null
} else { $env:KK_DEV_PAUSE_INPUT=$InputSteps }
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Task032Window {
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[Task032Window]::SetProcessDPIAware() | Out-Null
function Capture-OwnedWindow($process,$path) {
 $process.Refresh()
 $hwnd=$process.MainWindowHandle
 if($hwnd -eq [IntPtr]::Zero) { throw 'Our game has no window' }
 $rect=New-Object Task032Window+RECT
 [Task032Window]::GetClientRect($hwnd,[ref]$rect) | Out-Null
 $bmp=New-Object System.Drawing.Bitmap ($rect.R-$rect.L),($rect.B-$rect.T)
 $graphics=[System.Drawing.Graphics]::FromImage($bmp)
 $dc=$graphics.GetHdc()
 $ok=[Task032Window]::PrintWindow($hwnd,$dc,3)
 $graphics.ReleaseHdc($dc); $graphics.Dispose()
 if(!$ok) { $bmp.Dispose(); throw 'PrintWindow failed' }
 $bmp.Save($path,[System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
}
$game=$null
try {
 while(!$game) {
  $guard=$null
  try {
   $guard=[IO.File]::Open($lock,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::Read)
   $reader=New-Object IO.StreamReader($guard,[Text.Encoding]::UTF8,$true,1024,$true)
   $state=$reader.ReadToEnd().Trim(); $reader.Dispose()
   if($state.StartsWith('user')) { throw 'USER LOCK: code work only' }
   if($state -eq 'free') {
    $others=@(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'" | Where-Object { $_.ExecutablePath -like 'F:\KK-native-renderer\runs\*' -or $_.ExecutablePath -like 'F:\KK-native-renderer\wt\*' })
    if(!$others.Count) {
     if(!(Get-Process kkbg2 -ErrorAction SilentlyContinue)) { throw 'kkbg2 required' }
     $bytes=[Text.Encoding]::ASCII.GetBytes("task032 $(Get-Date -Format o)`r`n")
     $guard.SetLength(0); $guard.Position=0; $guard.Write($bytes,0,$bytes.Length); $guard.Flush()
     $game=Start-Process -FilePath "$run\king_kong.exe" -ArgumentList $launch -WorkingDirectory $run -WindowStyle Hidden -PassThru
    }
   }
  } catch [IO.IOException] { }
  finally { if($guard) { $guard.Dispose() } }
  if(!$game) { Write-Output "Busy: $state; checking in 30 seconds"; Start-Sleep -Seconds 30 }
 }
 Write-Output "RUN $run PID $($game.Id)"
 $timer=[Diagnostics.Stopwatch]::StartNew()
 $captured=@{}
 while(!$game.HasExited -and $timer.Elapsed.TotalSeconds -lt $Seconds) {
  Start-Sleep -Milliseconds 250
  if(Test-Path -LiteralPath "$run\run.log") {
   foreach($marker in (Select-String -LiteralPath "$run\run.log" -Pattern 'KK dev: shot ([a-zA-Z0-9_-]+)')) {
    $name=$marker.Matches[0].Groups[1].Value
    if(!$captured.ContainsKey($name)) {
     Start-Sleep -Milliseconds 300
     Capture-OwnedWindow $game "$run\ui-$name.png"
     $captured[$name]=$true
    }
   }
  }
 }
} finally {
 if($game -and !$game.HasExited) { $null=$game.CloseMainWindow(); if(!$game.WaitForExit(10000)) { Stop-Process -Id $game.Id; $game.WaitForExit() } }
 if((Get-Content -LiteralPath $lock -Raw).Trim().StartsWith('task032 ')) { Set-Content -LiteralPath $lock -Value 'free' -Encoding ascii }
}
Write-Output "DONE $run"
