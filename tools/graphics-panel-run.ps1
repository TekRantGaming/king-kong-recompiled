# The graphics panel's robustness test in the game (KK_DEV_PANEL_TEST, kk/src/debug_overlay.cpp), following
# codex/GAME-RULES.md: the shared lock, a private copy under runs/claude-overlay-*, windowed, kkbg2, own PIDs only.
# Summarise a run with tools/graphics-panel-summary.py <run folder>.
param([ValidateSet('vulkan','d3d12')][string]$Api='vulkan',
      [string]$Upscaler='dlss', [int]$Chapter=8, [int]$Toggles=100, [double]$Interval=0.1,
      [string]$Only='', [string]$Upscalers='', [int]$MaxSeconds=600, [switch]$Validation, [switch]$Verify)
$ErrorActionPreference='Stop'
$workspace='F:\KK-native-renderer\wt\codex-debugmenu'
$lock='F:\KK-native-renderer\runs\codex-game.lock'
$priority='F:\KK-native-renderer\runs\codex-game.priority'
$me='claude-overlay'
if((Get-Content -LiteralPath $lock -Raw).Trim().StartsWith('user')) { throw 'USER LOCK: code work only' }
$run="F:\KK-native-renderer\runs\claude-overlay-$(Get-Date -Format yyyyMMdd-HHmmss)-$Api-$Upscaler$(if($Verify){'-verify'})"
New-Item -ItemType Directory -Path "$run\cache\native-shaders" -Force | Out-Null
$build="$workspace\kk\out\build\kk-dev-vulkan"
Get-ChildItem -LiteralPath $build -File | Where-Object { $_.Name -eq 'king_kong.exe' -or $_.Extension -eq '.dll' } | Copy-Item -Destination $run
Copy-Item -LiteralPath "$build\glyphs" -Destination $run -Recurse
Copy-Item -LiteralPath 'F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc\bin\x64\dxcompiler.dll' -Destination $run
Copy-Item -Path 'F:\KK-native-renderer\runs\task026-20261013-211423\cache\native-shaders\kkshaders-*.pack*' -Destination "$run\cache\native-shaders"
robocopy 'F:\KK-native-renderer\userdata' "$run\userdata" /E /NFL /NDL /NJH /NJS /NP | Out-Null
if($LASTEXITCODE -ge 8) { throw 'Private user data copy failed' }
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
$env:KK_DEV_AUTOSKIP='1'; $env:KK_DEV_SCRIPT='0:SKIP/4'
$env:KK_DEV_PANEL_TEST="$run\captures"; New-Item -ItemType Directory -Path "$run\captures" -Force | Out-Null
$env:KK_DEV_PANEL_TOGGLES="$Toggles"; $env:KK_DEV_PANEL_INTERVAL="$Interval"; $env:KK_DEV_PANEL_UPSCALER=$Upscaler
$env:KK_DEV_PANEL_ONLY=$Only; $env:KK_DEV_PANEL_UPSCALERS=$Upscalers
$env:KK_DEV_EFFECT_TEST='1'
# -Verify: three captures of each value of each setting between captures of its own value (no input: a still camera).
if($Verify) { $env:KK_DEV_PANEL_VERIFY='1' } else { $env:KK_DEV_PANEL_VERIFY=$null; $env:KK_DEV_FORCE_DT=$null; $env:KK_DEV_FORCE_DT_AT=$null }
$env:KK_DEV_DT_LOG='0,1200'
$launch=@('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
 '--kk_renderer=native',"--native_backend=$Api", "--native_upscaler=$Upscaler",'--kk_frame_rate=30',
 "--kk_start=chapter:0:$Chapter",'--kk_launcher=false',"--user_data_root=$run\userdata",
 "--game_data_root=$workspace/kk/assets","--log_file=$run\run.log","--cache_root=$run\cache",
 "--config=$run\king_kong.toml",'--native_ao_method=gtao','--native_ao_apply=opaque',
 '--native_stutter_stats=true','--kk_check_updates=false','--kk_online_count=false','--kk_discord_status=false')
if($Validation) { $launch+='--native_validation=true' }
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class PanelWindow {
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[PanelWindow]::SetProcessDPIAware() | Out-Null
function Capture-Own($process,$path) {
 $process.Refresh()
 $rect=New-Object PanelWindow+RECT
 [PanelWindow]::GetClientRect($process.MainWindowHandle,[ref]$rect) | Out-Null
 if($rect.R -le 0 -or $rect.B -le 0) { return }
 $bmp=New-Object System.Drawing.Bitmap $rect.R,$rect.B
 $g=[System.Drawing.Graphics]::FromImage($bmp); $dc=$g.GetHdc()
 $ok=[PanelWindow]::PrintWindow($process.MainWindowHandle,$dc,3)
 $g.ReleaseHdc($dc); $g.Dispose()
 if($ok) { $bmp.Save($path,[System.Drawing.Imaging.ImageFormat]::Png) }
 $bmp.Dispose()
}
function Key($process,[int]$vk,[int]$scan,[bool]$down) {
 $l = 1 -bor ($scan -shl 16)
 if(!$down) { $l = $l -bor 0xC0000000 }
 [PanelWindow]::PostMessage($process.MainWindowHandle, $(if($down){0x100}else{0x101}), [IntPtr]$vk, [IntPtr][int64]$l) | Out-Null
}
function Read-Log { try { $s=[IO.File]::Open("$run\run.log",[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite); $r=[IO.StreamReader]::new($s); $t=$r.ReadToEnd(); $r.Dispose(); return $t } catch { return '' } }
$game=$null; $renderGame=$null
$samples=[Collections.Generic.List[object]]::new()
try {
 while(!$game) {
  if((Get-Content -LiteralPath $lock -Raw).Trim().StartsWith('user')) { throw 'USER LOCK: code work only' }
  if((Test-Path $priority) -and (Get-Content -LiteralPath $priority -Raw) -notmatch $me) { Write-Output 'Priority elsewhere; waiting'; Start-Sleep 30; continue }
  $others=@(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'")
  $state=(Get-Content -LiteralPath $lock -Raw).Trim()
  if($state -eq 'free' -and !$others.Count) {
   if(!(Get-Process kkbg2 -ErrorAction SilentlyContinue)) { throw 'kkbg2 required' }
   Set-Content -LiteralPath $lock -Value "$me $(Get-Date -Format o)" -Encoding ascii
   $game=Start-Process -FilePath "$run\king_kong.exe" -ArgumentList $launch -WorkingDirectory $run -WindowStyle Hidden -PassThru
  } else { Write-Output "Game slot busy: $state ($($others.Count) running)"; Start-Sleep 30 }
 }
 Write-Output "RUN $run PID $($game.Id)"
 $timer=[Diagnostics.Stopwatch]::StartNew(); $done=$false; $shown=[Collections.Generic.HashSet[string]]::new()
 while(!$game.HasExited -and $timer.Elapsed.TotalSeconds -lt $MaxSeconds) {
  Start-Sleep -Seconds 1
  $child=Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'" | Where-Object { $_.ParentProcessId -eq $game.Id -and $_.ExecutablePath -eq "$run\king_kong.exe" } | Select-Object -First 1
  if($child -and !$renderGame) { $renderGame=Get-Process -Id $child.ProcessId }
  if(!$renderGame) { continue }
  $renderGame.Refresh()
  $samples.Add([pscustomobject]@{seconds=[int]$timer.Elapsed.TotalSeconds;private_mb=[int]($renderGame.PrivateMemorySize64/1MB);ws_mb=[int]($renderGame.WorkingSet64/1MB)})
  $log=Read-Log
  # The test's 'show' moments: a picture of the window (PrintWindow of our own process, the panel included).
  foreach($m in [regex]::Matches($log,'KK panel test: show (\w+)')) {
   $name=$m.Groups[1].Value
   if(!$shown.Contains($name)) { [void]$shown.Add($name); Start-Sleep -Milliseconds 700; Capture-Own $renderGame "$run\show-$name.png" }
  }
  if($log -match 'KK panel test: done') {
   $done=$true
   for($i=0;$i -lt 8;$i++){ Start-Sleep 1; $renderGame.Refresh(); $samples.Add([pscustomobject]@{seconds=[int]$timer.Elapsed.TotalSeconds;private_mb=[int]($renderGame.PrivateMemorySize64/1MB);ws_mb=[int]($renderGame.WorkingSet64/1MB)}) }
   break
  }
 }
 $samples | Export-Csv -LiteralPath "$run\process-memory.csv" -NoTypeInformation
 Write-Output "Completed: $done after $([int]$timer.Elapsed.TotalSeconds) s"
} finally {
 if($renderGame -and !$renderGame.HasExited) { Stop-Process -Id $renderGame.Id; $renderGame.WaitForExit() }
 if($game -and !$game.HasExited) { Stop-Process -Id $game.Id; $game.WaitForExit() }
 Start-Sleep 2
 Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'" | Where-Object { $_.ExecutablePath -eq "$run\king_kong.exe" } | ForEach-Object { Stop-Process -Id $_.ProcessId -ErrorAction SilentlyContinue }
 if((Get-Content -LiteralPath $lock -Raw).Trim().StartsWith("$me ")) { Set-Content -LiteralPath $lock -Value 'free' -Encoding ascii }
}
Write-Output "DONE $run"
