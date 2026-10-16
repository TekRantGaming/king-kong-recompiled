param(
 [ValidateSet('route','chapter')][string]$Mode='route',
 # route: dev | beta (this worktree's builds). chapter: beta3 | beta4 | beta5 | beta6.
 [string]$Build='dev',
 [int]$Passes=3,
 [double]$Settle=-1,
 [int]$Entry=2,
 [int]$Seconds=50,
 [string]$Label='',
 [string]$Env='',          # extra NAME=value;NAME=value for developer aids
 [switch]$ShotLauncher     # route: capture the launcher's results page after the run
)
# Task 147 game runner: one launch per call, GAME-RULES windowed/private, the
# shared lock held only while this launch's own processes run.
$ErrorActionPreference='Stop'
$workspace='F:\KK-native-renderer\wt\bench-steady'
. "$workspace/codex/game-slot.ps1"
$task='claude-bench147'
$ledger="$workspace/kk/out/task147/launches.jsonl"
if((Read-GameSlotState).StartsWith('user')) {throw 'USER LOCK: no launch or wait; code-only work'}
$stamp=Get-Date -Format yyyyMMdd-HHmmss
$name=if($Mode -eq 'route'){"route-$Build-p$Passes"}else{"chapter-$Build-e$Entry"}
if($Label){$name+="-$Label"}
$run="F:\KK-native-renderer\runs\task147-$name-$stamp"
New-Item -ItemType Directory -Path $run -Force | Out-Null
$packs=@{
 'old'='F:\KK-native-renderer\runs\task106-channel-20261015-124330\cache\native-shaders\kkshaders-spirv.pack';
 'new'='F:\KK-native-renderer\runs\task144-baseline-20261016-122818\cache\native-shaders\kkshaders-spirv.pack'}
switch($Build) {
 'dev'   {$src="$workspace/kk/out/build/kk-dev-vulkan"; $pack=$packs.new; $pkg='F:\KK-native-renderer\testbuilds\beta6-test6-1016-1201\KingKong'}
 'beta'  {$src="$workspace/kk/out/build/kk-beta"; $pack=$packs.new; $pkg='F:\KK-native-renderer\testbuilds\beta6-test6-1016-1201\KingKong'}
 'beta6' {$src="$workspace/kk/out/build/kk-beta"; $pack=$packs.new; $pkg='F:\KK-native-renderer\testbuilds\beta6-test6-1016-1201\KingKong'}
 'beta5' {$src='F:\KK-native-renderer\testbuilds\v2.0.0-beta.5-release\KingKong'; $pack=$packs.old; $pkg=$src}
 'beta4' {$src='F:\KK-native-renderer\testbuilds\v2.0.0-beta.4-release\KingKong'; $pack=$packs.old; $pkg=$src}
 'beta3' {$src='F:\KK-native-renderer\testbuilds\v2.0.0-beta.3-1013-0653\KingKong'; $pack=$packs.old; $pkg=$src}
 default {throw "unknown build $Build"}
}
# The package's runtime files (VC runtimes, glyphs, DXC, pipeline list), then the build's exe and DLLs.
Get-ChildItem -LiteralPath $pkg -File | Where-Object {$_.Name -match '\.(dll|list)$'} | Copy-Item -Destination $run
Copy-Item -LiteralPath "$pkg/glyphs" -Destination "$run/glyphs" -Recurse
Copy-Item -LiteralPath "$src/king_kong.exe" -Destination $run -Force
Get-ChildItem -LiteralPath $src -Filter '*.dll' | Where-Object {$_.Name -notmatch '^rexgpu-(xenos|classic)'} | Copy-Item -Destination $run -Force
Copy-Item -LiteralPath $pack -Destination "$run/kkshaders-spirv.pack"
Copy-Item -LiteralPath 'F:\KK-native-renderer\testbuilds\bench-routes\route-45s-1015-1609.kkbr' -Destination "$run/benchmark-route.kkbr"
robocopy 'F:\KK-native-renderer\userdata' "$run/userdata" /E /NFL /NDL /NJH /NJS /NP | Out-Null
if($LASTEXITCODE -gt 7){throw 'Private userdata copy failed'}
# One pipeline cache per build, kept between this task's runs (as a player's would be).
$cache="F:\KK-native-renderer\runs\task147-cache-$(if($Build -eq 'dev' -or $Build -eq 'beta'){'beta6'}else{$Build})"
New-Item -ItemType Directory -Path $cache -Force | Out-Null
@'
kk_settings_version = 3
kk_render_quality = "custom"
gpu_plugin = "native"
kk_camera_sensitivity = 150
kk_deadzone = 5
kk_vibration_strength = 100
'@ | Set-Content -LiteralPath "$run/king_kong.toml" -Encoding ASCII
Get-ChildItem Env: | Where-Object {$_.Name -like 'KK_*' -or $_.Name -like 'REX_DEV_*'} |
 ForEach-Object {[Environment]::SetEnvironmentVariable($_.Name,$null,'Process')}
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
foreach($pair in ($Env -split ';' | Where-Object {$_})) { $k,$v=$pair -split '=',2; [Environment]::SetEnvironmentVariable($k,$v,'Process') }
# The same requested settings for every build: 1280x720 window and output, 2x internal (2560x1440),
# upscaler off, GTAO, GI off (Beta 3 has none), unfinished effects off, uncapped, VSync off.
$arguments=@('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
 "--user_data_root=$run/userdata","--game_data_root=$workspace/kk/assets",
 "--cache_root=$cache","--log_file=$run/run.log","--config=$run/king_kong.toml",
 '--kk_renderer=native','--gpu_plugin=native','--kk_original_look=false','--kk_frame_rate=0','--vsync=false',
 '--video_mode_width=1280','--video_mode_height=720','--kk_render_quality=custom',
 '--draw_resolution_scale_x=2','--draw_resolution_scale_y=2','--native_render_scale=1',
 '--native_upscaler=off','--native_ao_method=gtao','--ao_mode=1','--ao_strength=3',
 '--kk_check_updates=false','--kk_online_count=false','--kk_discord_status=false')
if($Build -ne 'beta3') {
 $arguments+=@('--native_backend=vulkan','--native_gi=off','--native_shadow_quality=original',
  '--native_water_reflections=original','--native_volumetrics=off')
}
if($Mode -eq 'route') {
 $arguments+=@('--kk_startup_child=true','--kk_launcher=false','--kk_benchmark=true',"--kk_benchmark_passes=$Passes")
 if($Settle -ge 0){$arguments+="--kk_benchmark_settle=$Settle"}
} else {
 $arguments+=@('--kk_skip_launcher=true',"--kk_start=chapter:0:$Entry")
}
$arguments | Set-Content -LiteralPath "$run/arguments.txt"
"$Env" | Set-Content -LiteralPath "$run/environment.txt"
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Task147Shot {
 [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr v);
 [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr w, IntPtr dc, uint f);
 [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr w, out Rect r);
 [StructLayout(LayoutKind.Sequential)] struct Rect { public int l,t,r,b; }
 public static bool Capture(IntPtr w,string path) {
  if(w==IntPtr.Zero)return false;
  var old=SetThreadDpiAwarenessContext(new IntPtr(-4));
  try {
   Rect r;if(!GetWindowRect(w,out r)||r.r<=r.l||r.b<=r.t)return false;
   using(var b=new System.Drawing.Bitmap(r.r-r.l,r.b-r.t))
   using(var g=System.Drawing.Graphics.FromImage(b)) {
    var dc=g.GetHdc();bool ok;
    try {ok=PrintWindow(w,dc,2);}finally{g.ReleaseHdc(dc);}
    if(ok)b.Save(path,System.Drawing.Imaging.ImageFormat.Png);
    return ok;
   }
  }finally{SetThreadDpiAwarenessContext(old);}
 }
}
'@
Get-FileHash -LiteralPath "$run/king_kong.exe","$run/rexgpu-native.dll","$run/rexruntime.dll","$run/kkshaders-spirv.pack" |
 Select-Object Path,Hash | ConvertTo-Json | Set-Content -LiteralPath "$run/input-hashes.json"
function Own-Games { @(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'" | Where-Object {$_.ExecutablePath -and $_.ExecutablePath.StartsWith($run,'OrdinalIgnoreCase')}) }
function Read-Log($path) {
 if(!(Test-Path -LiteralPath $path)){return ''}
 $stream=[IO.File]::Open($path,'Open','Read','ReadWrite'); $reader=[IO.StreamReader]::new($stream)
 try {$reader.ReadToEnd()} finally {$reader.Dispose()}
}
$guard=Wait-GameSlot -Task $task -RequireBackgroundHelper
# The shared rule: re-read after 2 s and make sure the claim is still ours.
Start-Sleep -Seconds 2
if((Read-GameSlotState) -ne $guard.Token){Release-GameSlot $guard; throw 'Lost the game lock race; retry later'}
$game=$null; $complete=$false; $started=Get-Date; $shots=@(); $pids=@()
try {
 $others=@(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'")
 if($others.Count){throw 'A game appeared after slot acquisition'}
 $rtss=@(Get-Process RTSS,RTSSHooksLoader64,EncoderServer -ErrorAction SilentlyContinue)
 [pscustomobject]@{Run=$run;Mode=$Mode;Build=$Build;Passes=$Passes;Entry=$Entry;Started=$started.ToString('o');RTSS=$rtss.Count} |
  ConvertTo-Json -Compress | Add-Content -LiteralPath $ledger
 $game=Start-Process -FilePath "$run/king_kong.exe" -ArgumentList $arguments -WorkingDirectory $run -WindowStyle Hidden -PassThru
 $pids+=$game.Id
 Write-Output "LAUNCHED PID=$($game.Id) $run"
 $limit=if($Mode -eq 'route'){[Math]::Min(14,2+4.2*$Passes)}else{4}
 $deadline=(Get-Date).AddMinutes($limit); $last=''; $loadedAt=$null; $doneAt=$null; $overlay=$false
 while((Get-Date) -lt $deadline) {
  if(@(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'" | Where-Object {!$_.ExecutablePath -or !$_.ExecutablePath.StartsWith($run,'OrdinalIgnoreCase')}).Count) {throw 'Another game appeared; stopping only ours'}
  foreach($p in Own-Games){ if($pids -notcontains $p.ProcessId){$pids+=[int]$p.ProcessId; Write-Output "OWN RELAUNCH PID=$($p.ProcessId)"} }
  $log=Read-Log "$run/run.log"
  $progress=([regex]::Matches($log,'[^\r\n]*(?:KK benchmark replay: pass|KK benchmark: complete|KK benchmark: cancelled|start request.s level is loaded)[^\r\n]*') | Select-Object -Last 1).Value
  if($progress -and $progress -ne $last){Write-Output $progress;$last=$progress}
  if($Mode -eq 'route') {
   if(!$overlay -and $log -match 'replay: pass 1 chapter 1' -and ((Get-Date)-$started).TotalSeconds -gt 40) {
    $game.Refresh(); if([Task147Shot]::Capture($game.MainWindowHandle,"$run/overlay.png")){$shots+='overlay.png'}; $overlay=$true
   }
   if(!$doneAt -and $log.Contains('KK benchmark: complete,')) {$doneAt=Get-Date}
   if($doneAt) {
    if(!$ShotLauncher) {$complete=$true; break}
    $relaunch=@(Own-Games | Where-Object {$_.ProcessId -ne $game.Id})
    if($relaunch.Count -and ((Get-Date)-$doneAt).TotalSeconds -gt 14) {
     $p=Get-Process -Id $relaunch[0].ProcessId -ErrorAction SilentlyContinue
     if($p -and $p.MainWindowHandle -ne 0 -and [Task147Shot]::Capture($p.MainWindowHandle,"$run/launcher-results.png")){$shots+='launcher-results.png'}
     $complete=$true; break
    }
    if(((Get-Date)-$doneAt).TotalSeconds -gt 60) {$complete=$true; Write-Output 'No relaunch window seen'; break}
   }
  } else {
   if(!$loadedAt -and $log -match "start request.s level is loaded") {$loadedAt=Get-Date}
   if($loadedAt -and ((Get-Date)-$loadedAt).TotalSeconds -ge $Seconds) {$complete=$true; break}
   if($game.HasExited) {break}
  }
  Start-Sleep -Milliseconds 1000
 }
} finally {
 foreach($p in Own-Games){ if($pids -notcontains $p.ProcessId){$pids+=[int]$p.ProcessId} }
 foreach($id in $pids){ if(Get-Process -Id $id -ErrorAction SilentlyContinue){Stop-Process -Id $id -ErrorAction SilentlyContinue; Wait-Process -Id $id -Timeout 20 -ErrorAction SilentlyContinue} }
 Release-GameSlot $guard
 [pscustomobject]@{Run=$run;Pids=$pids;Complete=$complete;Seconds=((Get-Date)-$started).TotalSeconds;Mode=$Mode;Build=$Build;Passes=$Passes;Entry=$Entry;Shots=$shots} |
  ConvertTo-Json | Set-Content -LiteralPath "$run/test-result.json"
}
if(!$complete){throw "Test incomplete: $run"}
Write-Output "COMPLETE $run"
