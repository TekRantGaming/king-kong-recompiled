param(
 [Parameter(Mandatory)][string]$Label,
 [ValidateSet('vulkan','d3d12')][string]$Api='vulkan',
 [int]$Cap=0,
 [switch]$MonitorOff,
 [switch]$Shots,
 [string]$Previous='',
 [string]$Build='F:\KK-native-renderer\wt\bench-rec\kk\out\build\kk-dev-vulkan'
)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\game-slot.ps1"
$task='task117'
$ledger='F:\KK-native-renderer\runs\task117-launches.txt'
if ((Test-Path -LiteralPath $ledger) -and @(Get-Content -LiteralPath $ledger).Count -ge 4) { throw 'Four-run limit reached' }
$run="F:\KK-native-renderer\runs\task117-$Label-"+(Get-Date -Format yyyyMMdd-HHmmss)
New-Item -ItemType Directory -Path $run | Out-Null
$base='F:\KK-native-renderer\testbuilds\beta6-test2-1015-1649\KingKong'
& robocopy $base $run /E /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -gt 7) { throw 'package copy failed' }
Copy-Item -LiteralPath "$Build\king_kong.exe","$Build\rexgpu-native.dll","$Build\rexruntime.dll" -Destination $run
$packs='F:\KK-native-renderer\runs\task113-current-vulkan-20261015-151840'
Copy-Item -LiteralPath "$packs\kkshaders-spirv.pack","$packs\kkshaders-dxil.pack" -Destination $run
Copy-Item -LiteralPath 'F:\KK-native-renderer\testbuilds\bench-routes\route-45s-1015-1609.kkbr' -Destination "$run\benchmark-route.kkbr"
& robocopy 'F:\KK-native-renderer\userdata' "$run\userdata" /E /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -gt 7) { throw 'userdata copy failed' }
$gameArgs=@('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
 '--kk_renderer=native',"--native_backend=$Api", "--kk_frame_rate=$Cap",'--vsync=false',
 '--kk_launcher=false','--kk_benchmark=true','--kk_startup_child=true',
 "--user_data_root=$run\userdata",'--game_data_root=F:/KK-native-renderer/wt/bench-rec/kk/assets',
 "--cache_root=$run\cache","--log_file=$run\run.log","--config=$run\king_kong.toml",
 '--kk_check_updates=false','--kk_online_count=false','--kk_discord_status=false',
 '--video_mode_width=1280','--video_mode_height=720','--draw_resolution_scale_x=1','--draw_resolution_scale_y=1',
 '--kk_render_quality=native','--native_upscaler=off','--native_gi=high',
 '--native_ao_method=gtao','--native_ao_samples=3','--ao_mode=1','--ao_strength=3',
 '--native_shadow_quality=original','--native_water_reflections=original','--native_volumetrics=off')
$gameArgs | Set-Content -LiteralPath "$run\arguments.txt"
Get-FileHash -Algorithm SHA256 -LiteralPath "$run\king_kong.exe","$run\rexgpu-native.dll","$run\rexruntime.dll","$run\benchmark-route.kkbr" | ConvertTo-Json | Set-Content -LiteralPath "$run\input-hashes.json"
Get-ChildItem Env: | Where-Object { $_.Name -like 'KK_DEV_*' -or $_.Name -eq 'KK_AUTOPLAY' } | ForEach-Object { [Environment]::SetEnvironmentVariable($_.Name,$null,'Process') }
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'
$env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
if ($MonitorOff) { $env:KK_DEV_BENCH_MONITOR_OFF='1' }
if ($Shots) { $env:KK_DEV_BENCH_UI='1' }
if ($Previous) {
 $previousResult=Get-ChildItem -LiteralPath "$Previous\userdata\benchmarks" -Filter results.txt -Recurse | Select-Object -First 1
 if (!$previousResult) { throw 'Previous results missing' }
 & robocopy $previousResult.DirectoryName "$run\userdata\benchmarks\$($previousResult.Directory.Name)" /E /NFL /NDL /NJH /NJS | Out-Null
 if ($LASTEXITCODE -gt 7) { throw 'Previous result copy failed' }
}
$shotRoot="F:\KK-native-renderer\documentary\benchmark-ui\$Label"
if ($Shots) { New-Item -ItemType Directory -Path $shotRoot -Force | Out-Null }
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Bench117Shot {
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
    if(ok) b.Save(path, System.Drawing.Imaging.ImageFormat.Png);
   }
  } finally { SetThreadDpiAwarenessContext(previous); }
 }
}
"@
$guard=Wait-GameSlot -Task $task -RequireBackgroundHelper
$owned=@()
try {
 if (Get-Process king_kong -ErrorAction SilentlyContinue) { throw 'A game appeared after claiming slot; do not launch' }
 $p=Start-Process -FilePath "$run\king_kong.exe" -ArgumentList $gameArgs -WorkingDirectory $run -WindowStyle Hidden -PassThru
 $owned+=$p.Id
 "$Label $Api cap=$Cap pid=$($p.Id) $run" | Add-Content -LiteralPath $ledger
 Write-Output "LAUNCHED $($p.Id) $run"
 $deadline=(Get-Date).AddMinutes(7)
 $lastProgress=''
 $result=$null
 $seen=@{}
 $markerAt=Get-Date
 $lastMarker=''
 $finishedAt=$null
 while ((Get-Date) -lt $deadline) {
  $games=@(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'")
  $children=@($games | Where-Object { $_.ParentProcessId -in $owned -and $_.ExecutablePath -eq "$run\king_kong.exe" })
  foreach ($c in $children) { if ($owned -notcontains $c.ProcessId) { $owned+=$c.ProcessId } }
  if ($games | Where-Object { $_.ProcessId -notin $owned }) { throw 'Another game appeared; stopping only this test process' }
  $result=Get-ChildItem -LiteralPath "$run\userdata\benchmarks" -Filter frame-times.svg -Recurse -ErrorAction SilentlyContinue | Where-Object { !$Previous -or $_.Directory.Name -ne $previousResult.Directory.Name } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if ($result -and !$finishedAt) { $finishedAt=Get-Date }
  if ($result -and (!$Shots -or ((Get-Date)-$finishedAt).TotalSeconds -gt 29)) { break }
  if ($Shots -and (Test-Path -LiteralPath "$run\run.log")) {
   $stream=[IO.File]::Open("$run\run.log",'Open','Read','ReadWrite')
   $reader=[IO.StreamReader]::new($stream)
   try { $log=$reader.ReadToEnd() } finally { $reader.Dispose() }
   $markers=@([regex]::Matches($log,'KK benchmark UI shot: (start|live-\d+|result-\d+)'))
   if ($markers.Count) {
    $marker=$markers[-1].Groups[1].Value
    if ($marker -ne $lastMarker) { $markerAt=Get-Date; $lastMarker=$marker }
    if (!$seen.ContainsKey($marker)) {
     Start-Sleep -Milliseconds 300
     foreach ($ownedId in $owned) {
      $window=Get-Process -Id $ownedId -ErrorAction SilentlyContinue
      if ($window -and $window.MainWindowHandle -ne 0) {
       [Bench117Shot]::Capture($window.MainWindowHandle,"$shotRoot\$marker.png")
       $seen[$marker]=$true
      }
     }
    }
    if ($marker -like 'result-*' -and !$seen.ContainsKey("$marker-details") -and ((Get-Date)-$markerAt).TotalSeconds -gt 1.6) {
     Start-Sleep -Milliseconds 50
     foreach ($ownedId in $owned) {
      $window=Get-Process -Id $ownedId -ErrorAction SilentlyContinue
      if ($window -and $window.MainWindowHandle -ne 0) {
       [Bench117Shot]::Capture($window.MainWindowHandle,"$shotRoot\$marker-details.png")
       $seen["$marker-details"]=$true
      }
     }
    }
   }
  }
  if ($p.HasExited) { throw "replay exited before results: $run" }
  if (Test-Path -LiteralPath "$run\run.log") {
   $progress=(Select-String -LiteralPath "$run\run.log" -Pattern 'KK benchmark replay: chapter|KK benchmark: complete|KK benchmark: saving|KK play: uni_go loads level' | Select-Object -Last 1).Line
   if ($progress -and $progress -ne $lastProgress) { Write-Output $progress; $lastProgress=$progress }
  }
  Start-Sleep -Milliseconds $(if ($Shots) {100} else {1000})
 }
 if (!$result) { throw "no results within seven minutes: $run" }
 $resultDir=$result.DirectoryName
 Get-Content -LiteralPath "$resultDir\results.txt" | Select-Object -First 8
 [pscustomobject]@{Run=$run;Pid=$p.Id;OwnedPids=$owned;Results=$resultDir;Api=$Api;Cap=$Cap;MonitorOff=[bool]$MonitorOff;Shots=$seen.Keys} | ConvertTo-Json | Set-Content -LiteralPath "$run\test-result.json"
} finally {
 foreach ($own in $owned) { $proc=Get-Process -Id $own -ErrorAction SilentlyContinue; if ($proc) { Stop-Process -Id $own; Wait-Process -Id $own -ErrorAction SilentlyContinue } }
 Release-GameSlot $guard
}
