param([string]$Label='baseline', [int]$Chapter=8, [string]$Backend='vulkan',
 [switch]$Classic, [string]$Scale='1920x1080', [int]$Cores=0,
 [switch]$Gtao, [string]$Upscaler='off', [switch]$Fire, [switch]$PassTimers, [switch]$Validation,
 [string]$NativeDll='')
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$lockPath='F:\KK-native-renderer\runs\codex-game.lock'
$coreMask=0L
if ($Cores -gt 0) {
 Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Task012CoreMask {
 [DllImport("kernel32.dll", SetLastError=true)]
 static extern bool GetLogicalProcessorInformationEx(int relationship, IntPtr data, ref uint bytes);
 public static long Select(int cores) {
  uint bytes=0;
  GetLogicalProcessorInformationEx(0,IntPtr.Zero,ref bytes);
  IntPtr data=Marshal.AllocHGlobal((int)bytes);
  try {
   if (!GetLogicalProcessorInformationEx(0,data,ref bytes)) throw new System.ComponentModel.Win32Exception();
   int offset=0, selected=0; long mask=0;
   while (offset<bytes) {
    int size=Marshal.ReadInt32(data,offset+4);
    if (size<=0) throw new InvalidOperationException("Invalid CPU topology record");
    if (Marshal.ReadInt32(data,offset)==0 && Marshal.ReadInt16(data,offset+30)==1 && Marshal.ReadInt16(data,offset+40)==0) {
     mask |= Marshal.ReadInt64(data,offset+32);
     if (++selected==cores) return mask;
    }
    offset += size;
   }
   throw new InvalidOperationException("Requested core count is unavailable in processor group 0");
  } finally { Marshal.FreeHGlobal(data); }
 }
}
'@
 $coreMask=[Task012CoreMask]::Select($Cores)
}
function Wait-Free {
 if (!(Test-Path -LiteralPath $lockPath)) { Set-Content -LiteralPath $lockPath 'free' }
 while ($true) {
  try { $state=(Get-Content -LiteralPath $lockPath -Raw -ErrorAction Stop).Trim() } catch { $state='busy' }
  $ours=Get-Process king_kong -ErrorAction SilentlyContinue | Where-Object { $_.Path -like 'F:\KK-native-renderer\runs\*' -or $_.Path -like 'F:\KK-native-renderer\wt\*' }
  if ($state -eq 'free' -and !$ours) { break }
  Start-Sleep -Seconds 30
 }
 if (!(Get-Process kkbg2 -ErrorAction SilentlyContinue)) { throw 'kkbg2 is required.' }
}
$run="F:\KK-native-renderer\runs\task012-$Label-ch$Chapter-$(Get-Date -Format yyyyMMdd-HHmmss)"
New-Item -ItemType Directory -Path $run | Out-Null
$build=Join-Path $workspace 'kk/out/build/kk-dev-vulkan'
Get-ChildItem -LiteralPath $build -File | Where-Object { $_.Extension -in '.exe','.dll' } | Copy-Item -Destination $run
if ($NativeDll) { Copy-Item -LiteralPath $NativeDll -Destination "$run\rexgpu-native.dll" -Force }
Copy-Item -LiteralPath "$build\glyphs" -Destination $run -Recurse
Get-ChildItem 'F:\KK-native-renderer\wt\beta-upscalers\kk\out\build\kk-dev-vulkan\kkshaders-*.pack' | Copy-Item -Destination $run
Copy-Item 'F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc\bin\x64\dxcompiler.dll' $run
robocopy F:\KK-native-renderer\userdata "$run\userdata" /E /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'User data copy failed.' }
Get-ChildItem Env: | Where-Object { $_.Name -like 'KK_DEV_*' -or $_.Name -like 'REX_DEV_*' } | ForEach-Object { Remove-Item "Env:$($_.Name)" }
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'
$env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
$env:KK_DEV_AUTOSKIP='1'
$env:KK_DEV_DT_LOG='0,240'
$env:KK_DEV_SCRIPT='0:SKIP/120'
$env:KK_DEV_SHOTS='100,120'
$env:KK_DEV_SHOTS_FROM='menu'
$env:KK_DEV_SHOTS_DIR=$run
if ($Fire) { $env:KK_DEV_SCRIPT='4:LT+RSDOWN/117,10:RT/110,65:RSRIGHT/0.5'; $env:KK_DEV_GUNFIRE='1' }
$keep='kk_frame_rate,render_scale,native_render_scale,native_upscaler,ao_mode,ao_strength'
$debug=if ($PassTimers) {557056} else {32768}
$args=@('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
 "--kk_start=chapter:0:$Chapter",'--kk_launcher=false','--kk_cheats=true',
 "--user_data_root=$run\userdata","--game_data_root=$($workspace.Replace('\','/'))/kk/assets","--log_file=$run\run.log",
 "--cache_root=$run\cache",'--kk_original_look=true',"--kk_original_look_keep=$keep",'--kk_frame_rate=0',
 "--native_debug=$debug",'--native_stutter_stats=true',"--native_backend=$Backend",
 "--native_render_scale=$Scale", "--native_upscaler=$Upscaler",'--native_upscaler_gpu_time=true','--native_ao_gpu_time=true')
if ($Classic) { $args+='--kk_renderer=classic'; $args+="--draw_resolution_scale_x=$(if($Scale -eq '1920x1080'){1}else{2})"; $args+="--draw_resolution_scale_y=$(if($Scale -eq '1920x1080'){1}else{2})" }
if ($Gtao) { $args+='--ao_mode=1','--ao_strength=2','--native_ao_method=gtao' } else { $args+='--ao_mode=0' }
if ($Fire) { $args+='--kk_cheat_ammo=true','--kk_cheat_machine_gun=true','--mnk_mode=false','--kk_camera_sensitivity=400' }
if ($Validation) { $args+='--native_validation=true' }
@{label=$Label;chapter=$Chapter;backend=$Backend;classic=[bool]$Classic;scale=$Scale;cores=$Cores;
 affinity_mask=$coreMask.ToString('X'); native_dll=$NativeDll; dll_sha256=(Get-FileHash "$run\rexgpu-native.dll").Hash; args=$args} | ConvertTo-Json | Set-Content "$run\test.json"
Wait-Free
Set-Content -LiteralPath $lockPath "task012 $(Get-Date -Format o)"
$game=$null
try {
 $game=Start-Process -FilePath "$run\king_kong.exe" -ArgumentList $args -WorkingDirectory $run -WindowStyle Hidden -PassThru
 if ($Cores -gt 0) { $game.ProcessorAffinity=[IntPtr]$coreMask }
 Write-Output "RUN $run PID $($game.Id)"
 'seconds,private_bytes,working_set,cpu_seconds' | Set-Content "$run\process.csv"
 'seconds,dedicated_bytes,shared_bytes' | Set-Content "$run\gpu-memory.csv"
 $memoryAt=0
 $clock=[Diagnostics.Stopwatch]::StartNew()
 while (!$game.HasExited -and $clock.Elapsed.TotalSeconds -lt 240) {
  $game.Refresh()
  "$($clock.Elapsed.TotalSeconds),$($game.PrivateMemorySize64),$($game.WorkingSet64),$($game.TotalProcessorTime.TotalSeconds)" | Add-Content "$run\process.csv"
  if ($clock.Elapsed.TotalSeconds -ge $memoryAt) {
   $memoryAt=$clock.Elapsed.TotalSeconds+10
   $gpu=Get-CimInstance -ClassName Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "pid_$($game.Id)_*" }
   "$($clock.Elapsed.TotalSeconds),$(($gpu | Measure-Object DedicatedUsage -Sum).Sum),$(($gpu | Measure-Object SharedUsage -Sum).Sum)" | Add-Content "$run\gpu-memory.csv"
  }
  if (Test-Path "$run\shot_1200.bmp") { break }
  Start-Sleep -Seconds 2
 }
} finally {
 if ($game -and !$game.HasExited) { $null=$game.CloseMainWindow(); if (!$game.WaitForExit(15000)) { Stop-Process -Id $game.Id; $game.WaitForExit() } }
 Set-Content -LiteralPath $lockPath 'free'
}
Write-Output "DONE $run"
