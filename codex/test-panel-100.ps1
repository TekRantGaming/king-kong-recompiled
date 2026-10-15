# One short, background-only controller panel run. At most two runs for task 100.
param([ValidateSet('vulkan','d3d12')][string]$Api='vulkan',
      [ValidateSet(1,3,4,5,6)][int]$Language=1)
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path "$PSScriptRoot\..").Path
$build="$workspace\kk\out\build\kk-dev-vulkan"
$lock='F:\KK-native-renderer\runs\codex-game.lock'
$priority='F:\KK-native-renderer\runs\codex-game.priority'
$me='task100'
if (!(Test-Path -LiteralPath $lock)) { Set-Content -LiteralPath $lock -Value 'free' -Encoding ascii }
if ((Get-Content -LiteralPath $lock -Raw).Trim().StartsWith('user')) { throw 'USER LOCK: code-only work; no game launch.' }
if (@(Get-ChildItem 'F:\KK-native-renderer\runs' -Directory -Filter 'task100-panel-*' |
      Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'launched.txt') }).Count -ge 2) {
  throw 'Task 100 has already used its two game runs.'
}
$run="F:\KK-native-renderer\runs\task100-panel-$Api-$(Get-Date -Format yyyyMMdd-HHmmss)"
New-Item -ItemType Directory -Path "$run\cache\native-shaders" -Force | Out-Null
Get-ChildItem -LiteralPath $build -File |
  Where-Object { $_.Name -eq 'king_kong.exe' -or $_.Extension -eq '.dll' } | Copy-Item -Destination $run
Copy-Item -LiteralPath "$build\glyphs" -Destination $run -Recurse
Copy-Item -Path 'F:\KK-native-renderer\runs\task099-vk-profile-20261015-110128\kkshaders-*.pack' -Destination "$run\cache\native-shaders"
robocopy 'F:\KK-native-renderer\userdata' "$run\userdata" /E /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'Private user data copy failed.' }
foreach ($dll in 'rexruntime.dll','rexgpu-native.dll','dxcompiler.dll','nvngx_dlss.dll','libxess.dll',
                 'amd_fidelityfx_vk.dll','amd_fidelityfx_dx12.dll','amd_fidelityfx_loader_dx12.dll','amd_fidelityfx_upscaler_dx12.dll') {
  if (!(Test-Path -LiteralPath "$run\$dll")) { throw "Missing $dll" }
}
$plugin=[IO.File]::ReadAllBytes("$run\rexgpu-native.dll")
$strings=[Text.Encoding]::ASCII.GetString($plugin)
if (!$strings.Contains('NVSDK_NGX') -or !$strings.ToLowerInvariant().Contains('xess')) { throw 'Missing DLSS/XeSS in native plugin.' }
@'
kk_settings_version = 3
gpu_plugin = "native"
kk_render_quality = "native"
native_upscaler = "off"
native_gi = "off"
swap_post_effect = "none"
'@ | Set-Content -LiteralPath "$run\king_kong.toml" -Encoding ascii

# PrintWindow of our owned child only, with physical pixel dimensions. No
# desktop capture, window activation, keyboard/mouse injection or foregrounding.
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Panel100Shot {
 [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr v);
 [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr w, IntPtr dc, uint f);
 [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr w, out Rect r);
 [StructLayout(LayoutKind.Sequential)] struct Rect { public int l,t,r,b; }
 public static void Capture(IntPtr w, string path) {
  var previous=SetThreadDpiAwarenessContext(new IntPtr(-4));
  try {
   Rect r; if(!GetWindowRect(w,out r) || r.r<=r.l || r.b<=r.t) return;
   using(var b=new System.Drawing.Bitmap(r.r-r.l,r.b-r.t))
   using(var g=System.Drawing.Graphics.FromImage(b)) {
    var dc=g.GetHdc(); bool ok;
    try { ok=PrintWindow(w,dc,2); } finally { g.ReleaseHdc(dc); }
    if(ok) b.Save(path);
   }
  } finally { SetThreadDpiAwarenessContext(previous); }
 }
}
'@
function Read-PanelLog {
  try {
    $stream=[IO.File]::Open("$run\run.log",'Open','Read','ReadWrite')
    $reader=[IO.StreamReader]::new($stream)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
  } catch { return '' }
}
$owned=[Collections.Generic.HashSet[int]]::new()
$game=$null
$claimed=$false
$completed=$false
try {
  while (!$claimed) {
    $state=(Get-Content -LiteralPath $lock -Raw).Trim()
    if ($state.StartsWith('user')) { throw 'USER LOCK: no game launch.' }
    if ((Test-Path -LiteralPath $priority) -and (Get-Content -LiteralPath $priority -Raw).Trim() -ne $me) {
      Write-Output 'Priority belongs to another task; waiting 30 seconds.'
      Start-Sleep -Seconds 30
      continue
    }
    $others=@(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'")
    if ($state -ne 'free' -or $others.Count) {
      $ownGame=@($others | Where-Object { $_.ExecutablePath -notlike 'F:\KK-native-renderer\runs\*' -and $_.ExecutablePath -notlike 'F:\KK-native-renderer\wt\*' })
      Write-Output "Game slot busy: $state; $($others.Count) processes."
      if ($ownGame.Count) { Start-Sleep -Seconds 60 } else { Start-Sleep -Seconds 30 }
      continue
    }
    if (!(Get-Process kkbg2 -ErrorAction SilentlyContinue)) { throw 'kkbg2 must already be running.' }
    Set-Content -LiteralPath $lock -Value "$me $(Get-Date -Format o)" -Encoding ascii
    $claimed=$true
  }
  $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'
  $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
  $env:KK_DEV_AUTOSKIP='1'
  $env:KK_DEV_SCRIPT='0:SKIP/4'
  $env:KK_DEV_PANEL_CONTROLLER='1'
  $env:KK_DEV_PANEL_TEST=$null
  $env:KK_DEV_PANEL_VERIFY=$null
  $env:KK_DEV_UIPAINT_TEST=$null
  $env:KK_DEV_WANDER=$null
  $args=@('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
    '--kk_renderer=native',"--native_backend=$Api",'--native_upscaler=off','--kk_frame_rate=60',
    '--kk_start=chapter:0:27','--kk_launcher=false',"--user_data_root=$run\userdata",
    "--game_data_root=$workspace/kk/assets","--log_file=$run\run.log","--cache_root=$run\cache",
    "--config=$run\king_kong.toml", "--user_language=$Language",'--kk_check_updates=false',
    '--kk_online_count=false','--kk_discord_status=false')
  $game=Start-Process -FilePath "$run\king_kong.exe" -WorkingDirectory $run -ArgumentList $args -WindowStyle Hidden -PassThru
  [void]$owned.Add($game.Id)
  Set-Content -LiteralPath "$run\launched.txt" -Value $game.Id -Encoding ascii
  Write-Output "RUN $run PID $($game.Id)"
  $timer=[Diagnostics.Stopwatch]::StartNew()
  $shot=$false
  $languages=[Collections.Generic.HashSet[string]]::new()
  while ($timer.Elapsed.TotalSeconds -lt 150) {
    foreach ($p in @(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'")) {
      if ($p.ExecutablePath -eq "$run\king_kong.exe" -and $owned.Contains([int]$p.ParentProcessId)) { [void]$owned.Add([int]$p.ProcessId) }
    }
    $log=Read-PanelLog
    if (!$shot -and $log.Contains('both stick clicks open the panel')) {
      foreach ($ownedId in $owned) {
        $p=Get-Process -Id $ownedId -ErrorAction SilentlyContinue
        if ($p -and $p.MainWindowHandle -ne 0) { [Panel100Shot]::Capture($p.MainWindowHandle,"$run\panel.png"); $shot=$true }
      }
    }
    foreach ($match in [regex]::Matches($log,'KK panel controller test: show language (\d+)')) {
      $languageId=$match.Groups[1].Value
      if ($languages.Add($languageId)) {
        Start-Sleep -Milliseconds 100
        foreach ($ownedId in $owned) {
          $p=Get-Process -Id $ownedId -ErrorAction SilentlyContinue
          if ($p -and $p.MainWindowHandle -ne 0) { [Panel100Shot]::Capture($p.MainWindowHandle,"$run\panel-language-$languageId.png") }
        }
      }
    }
    if ($log -match 'KK panel controller test: done: (\d+) passed, (\d+) failed') {
      $completed=$true
      Write-Output $matches[0]
      if ([int]$matches[2]) { throw 'Controller harness reported failures.' }
      break
    }
    Start-Sleep -Milliseconds 100
  }
  if (!$completed) { throw 'Controller harness did not finish within 150 seconds.' }
} finally {
  # Capture only descendants of the PID this runner started, at our own path.
  foreach ($p in @(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'")) {
    if ($p.ExecutablePath -eq "$run\king_kong.exe" -and $owned.Contains([int]$p.ParentProcessId)) { [void]$owned.Add([int]$p.ProcessId) }
  }
  foreach ($ownedId in $owned) {
    $p=Get-Process -Id $ownedId -ErrorAction SilentlyContinue
    if ($p -and !$p.HasExited) { Stop-Process -Id $ownedId; $p.WaitForExit() }
  }
  if ($claimed -and (Get-Content -LiteralPath $lock -Raw).Trim().StartsWith("$me ")) {
    Set-Content -LiteralPath $lock -Value 'free' -Encoding ascii
  }
  Write-Output "DONE $run"
}
