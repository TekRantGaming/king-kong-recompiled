param([ValidateSet('vulkan','d3d12')][string]$Api='vulkan',[int]$Chapter=2)
$ErrorActionPreference='Stop'
. "$PSScriptRoot/game-slot.ps1"
$workspace=(Resolve-Path "$PSScriptRoot/..").Path
$build="$workspace/kk/out/build/kk-beta"
$ledger='F:\KK-native-renderer\runs\task129-launches.txt'
if((Read-GameSlotState).StartsWith('user')){throw 'USER LOCK: code-only work; no game launch or wait'}
if((Test-Path $ledger) -and @(Get-Content $ledger).Count -ge 2){throw 'Two-run limit reached'}
$run='F:\KK-native-renderer\runs\task129-'+$Api+'-'+(Get-Date -Format yyyyMMdd-HHmmss)
New-Item -ItemType Directory -Path $run | Out-Null
$baseline='F:\KK-native-renderer\testbuilds\beta6-test3-1015-1729\KingKong'
& robocopy $baseline $run /E /XF TEST-CHECKLIST.txt /NFL /NDL /NJH /NJS | Out-Null
if($LASTEXITCODE -gt 7){throw 'Baseline copy failed'}
Copy-Item -LiteralPath "$build/king_kong.exe" -Destination $run
Get-ChildItem -LiteralPath $build -Filter '*.dll' | Copy-Item -Destination $run
$packs='F:\KK-native-renderer\runs\task113-current-vulkan-20261015-151840'
Copy-Item -LiteralPath "$packs/kkshaders-spirv.pack","$packs/kkshaders-dxil.pack" -Destination $run
& robocopy 'F:\KK-native-renderer\userdata' "$run/userdata" /E /NFL /NDL /NJH /NJS | Out-Null
if($LASTEXITCODE -gt 7){throw 'Private userdata copy failed'}
Get-ChildItem Env: | Where-Object {$_.Name -like 'KK_DEV_*' -or $_.Name -eq 'KK_AUTOPLAY'} |
 ForEach-Object {[Environment]::SetEnvironmentVariable($_.Name,$null,'Process')}
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0';$env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
$env:KK_DEV_FREECAM_TEST='1'
$gameArgs=@('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
 '--kk_renderer=native',"--native_backend=$Api",'--kk_frame_rate=60','--vsync=false',
 '--kk_launcher=false',"--kk_start=chapter:0:$Chapter",'--kk_startup_child=true',
 "--user_data_root=$run/userdata",('--game_data_root='+$workspace.Replace('\','/')+'/kk/assets'),
 "--cache_root=$run/cache","--log_file=$run/run.log","--config=$run/king_kong.toml",
 '--kk_check_updates=false','--kk_online_count=false','--kk_discord_status=false',
 '--video_mode_width=1280','--video_mode_height=720','--draw_resolution_scale_x=1','--draw_resolution_scale_y=1',
 '--kk_render_quality=native','--native_gi=medium','--kk_original_look=false','--kk_show_fps=true',
 '--native_shadow_quality=original','--native_water_reflections=original','--native_volumetrics=off',
 '--kk_cheats=true','--kk_cheat_healing=true','--kk_ps2_cutscenes=true',
 '--kk_ps2_movie_folder=C:/Users/TekRa/Desktop/kk_upscaled_movies')
$gameArgs+=if($Api -eq 'vulkan'){'--native_upscaler=dlss'}else{'--native_upscaler=fsr3'}
$gameArgs | Set-Content -LiteralPath "$run/arguments.txt"
$shots="F:\KK-native-renderer\documentary\freecam\$Api"
New-Item -ItemType Directory -Path $shots -Force | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Freecam129Shot {
 [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr v);
 [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr w,IntPtr dc,uint f);
 [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr w,out Rect r);
 [StructLayout(LayoutKind.Sequential)] struct Rect {public int l,t,r,b;}
 public static void Capture(IntPtr w,string path) {
  var old=SetThreadDpiAwarenessContext(new IntPtr(-4));
  try {
   Rect r;if(!GetWindowRect(w,out r)||r.r<=r.l||r.b<=r.t)return;
   using(var b=new System.Drawing.Bitmap(r.r-r.l,r.b-r.t))
   using(var g=System.Drawing.Graphics.FromImage(b)) {
    var dc=g.GetHdc();bool ok;
    try {ok=PrintWindow(w,dc,2);}finally{g.ReleaseHdc(dc);}
    if(ok)b.Save(path,System.Drawing.Imaging.ImageFormat.Png);
   }
  }finally{SetThreadDpiAwarenessContext(old);}
 }
}
'@
function Read-OwnLog {
 if(!(Test-Path -LiteralPath "$run/run.log")){return ''}
 $stream=[IO.File]::Open("$run/run.log",'Open','Read','ReadWrite');$reader=[IO.StreamReader]::new($stream)
 try{return $reader.ReadToEnd()}finally{$reader.Dispose()}
}
$guard=Wait-GameSlot -Task task129 -RequireBackgroundHelper
$owned=@()
try {
 if(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'"){throw 'A game appeared after slot claim'}
 if((Test-Path $ledger) -and @(Get-Content $ledger).Count -ge 2){throw 'Two-run limit reached'}
 $p=Start-Process -FilePath "$run/king_kong.exe" -ArgumentList $gameArgs -WorkingDirectory $run -WindowStyle Hidden -PassThru
 $owned+=$p.Id
 "$Api chapter=$Chapter pid=$($p.Id) $run" | Add-Content -LiteralPath $ledger
 Write-Output "LAUNCHED $($p.Id) $run"
 $deadline=(Get-Date).AddMinutes(3);$loaded=$null;$seen=@{};$done=$false
 while((Get-Date)-lt $deadline) {
  if(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'" | Where-Object {$_.ProcessId -notin $owned}){throw 'Another game appeared; stopping only our test'}
  if($p.HasExited){throw "Game exited early: $run"}
  $log=Read-OwnLog
  if(!$loaded -and $log -match 'the start request.s level is loaded'){$loaded=Get-Date}
  $p.Refresh()
  if($loaded -and $p.MainWindowHandle -ne 0) {
   foreach($s in @(2,6,8.7,12,14.7,16,19,23,25)) {
    if(!$seen.ContainsKey("$s") -and ((Get-Date)-$loaded).TotalSeconds -ge $s) {
     [Freecam129Shot]::Capture($p.MainWindowHandle,"$shots/frame-$s.png");$seen["$s"]=$true
    }
   }
  }
  if($log -match 'KK freecam test: DONE active=false hidden=false' -and $seen.ContainsKey('25')){$done=$true;break}
  Start-Sleep -Milliseconds 200
 }
 [pscustomobject]@{Run=$run;Pid=$p.Id;Api=$Api;Chapter=$Chapter;Loaded=[bool]$loaded;Completed=$done;Shots=$seen.Keys} |
  ConvertTo-Json | Set-Content -LiteralPath "$run/test-result.json"
 if(!$done){throw "Freecam test did not finish: $run"}
 Select-String -LiteralPath "$run/run.log" -Pattern 'KK freecam' | ForEach-Object Line
} finally {
 foreach($own in $owned) {if(Get-Process -Id $own -ErrorAction SilentlyContinue){Stop-Process -Id $own;Wait-Process -Id $own -ErrorAction SilentlyContinue}}
 Release-GameSlot $guard
}
