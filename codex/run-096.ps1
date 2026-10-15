param([Parameter(Mandatory)][string]$Label,
      [ValidateSet('vulkan','d3d12')][string]$Backend='vulkan',
      [ValidateSet('mailbox','fifo','immediate')][string]$Mode='mailbox',
      [int]$Cap=60,[switch]$Baseline)
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path "$PSScriptRoot/..").Path
$lockPath='F:\KK-native-renderer\runs\codex-game.lock'
$priorityPath='F:\KK-native-renderer\runs\codex-game.priority'
$countPath="$workspace/kk/out/task096/game-runs.json"
function Wait-GameSlot {
  while ($true) {
    if (!(Test-Path -LiteralPath $lockPath)) { Set-Content -LiteralPath $lockPath 'free' }
    $state=(Get-Content -LiteralPath $lockPath -Raw).Trim()
    if ($state.StartsWith('user')) { throw 'USER LOCK: no launch; code-only work remains.' }
    $priority=if(Test-Path $priorityPath){(Get-Content $priorityPath -Raw).Trim()}else{''}
    $games=@(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'")
    $owner=@($games | Where-Object {!$_.ExecutablePath -or ($_.ExecutablePath -notlike 'F:\KK-native-renderer\runs\*' -and $_.ExecutablePath -notlike 'F:\KK-native-renderer\wt\*')})
    if($state -eq 'free' -and !$games.Count -and (!$priority -or $priority -in '096','task096')) {return}
    Write-Output "Waiting: lock=$state priority=$priority games=$($games.Count)"
    Start-Sleep -Seconds $(if($owner.Count){60}else{30})
  }
}
function Read-SharedLog([string]$path) {
  if(!(Test-Path $path)){return ''}
  $stream=[IO.File]::Open($path,'Open','Read','ReadWrite')
  try { $reader=New-Object IO.StreamReader($stream); return $reader.ReadToEnd() }
  finally {if($reader){$reader.Dispose()}else{$stream.Dispose()}}
}
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Capture096 {
 [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
 [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
 [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hwnd, out Rect r);
 [StructLayout(LayoutKind.Sequential)] struct Rect {public int l,t,r,b;}
 public static void Save(IntPtr hwnd,string path) {
  var previous=SetThreadDpiAwarenessContext(new IntPtr(-4));
  try {
   Rect r; if(!GetWindowRect(hwnd,out r)) throw new Exception("Own window unavailable");
   using(var image=new System.Drawing.Bitmap(r.r-r.l,r.b-r.t))
   using(var g=System.Drawing.Graphics.FromImage(image)) {
    var dc=g.GetHdc();bool ok;try{ok=PrintWindow(hwnd,dc,2);}finally{g.ReleaseHdc(dc);}
    if(!ok)throw new Exception("Own PrintWindow failed");image.Save(path);
   }
  } finally {SetThreadDpiAwarenessContext(previous);}
 }
}
'@
Wait-GameSlot
if(!(Get-Process kkbg2 -ErrorAction SilentlyContinue)){throw 'kkbg2 is not running'}
function Flatten-Runs($node) {
  if($node -is [array]) {foreach($child in $node){Flatten-Runs $child}}
  elseif($node -and $node.PSObject.Properties['value']) {Flatten-Runs $node.value}
  elseif($node -and $node.PSObject.Properties['pid']) {$node}
}
$runs=@()
if(Test-Path $countPath){$runs=@(Flatten-Runs (Get-Content $countPath -Raw | ConvertFrom-Json))}
if($runs.Count -ge 4){throw 'Four-game-run limit reached'}
$run='F:\KK-native-renderer\runs\task096-'+$Label+'-'+(Get-Date -Format yyyyMMdd-HHmmss)
New-Item -ItemType Directory -Path $run | Out-Null
$build="$workspace/kk/out/build/kk-beta"
Get-ChildItem -LiteralPath $build -File | Where-Object {$_.Extension -in '.exe','.dll' -and (!$Baseline -or $_.Name -ne 'king_kong.exe')} | Copy-Item -Destination $run
if($Baseline){Copy-Item "$workspace/kk/out/baseline-096/king_kong.exe" $run}
Copy-Item "$workspace/kk/out/presentation-096/rexruntime.dll" $run -Force
Copy-Item "$build/glyphs" $run -Recurse
Copy-Item 'F:/KK-native-renderer/cache/native-shaders/kkshaders-spirv.pack' $run
Copy-Item 'F:/KK-native-renderer/runs/task064-20261015-002710/cache/native-shaders/kkshaders-dxil.pack' $run
Copy-Item 'F:/KK-native-renderer/testbuilds/beta5-test-1015-0739/KingKong/kkpipelines.list' $run
Copy-Item 'F:/KK-native-renderer/downloads/dxc-v1.9.2609/dxc/bin/x64/dxcompiler.dll' $run -Force
robocopy F:\KK-native-renderer\userdata "$run\userdata" /E /NFL /NDL /NJH /NJS /NP | Out-Null
if($LASTEXITCODE -ge 8){throw 'Private user data copy failed'}
Get-ChildItem Env: | Where-Object {$_.Name -like 'KK_DEV_*' -or $_.Name -like 'REX_DEV_*'} | ForEach-Object {Remove-Item "Env:$($_.Name)"}
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'
$env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
$args=@('--kk_startup_child=true','--kk_benchmark=true','--kk_launcher=false',
  '--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
  "--user_data_root=$run\userdata","--game_data_root=$($workspace.Replace('\','/'))/kk/assets",
  "--cache_root=$run\cache","--log_file=$run\run.log","--native_backend=$Backend", "--kk_frame_rate=$Cap",
  '--kk_original_look=true','--kk_original_look_keep=kk_frame_rate',
  '--kk_check_updates=false','--kk_online_count=false','--kk_discord_status=false',
  '--native_async_pipelines=false','--native_validation=false','--kk_show_fps=false','--ui_paint_trace=true')
$sync=$Mode -eq 'fifo'
$args+="--d3d12_allow_variable_refresh_rate_and_tearing=$((!$sync).ToString().ToLower())"
if($Backend -eq 'vulkan' -and $Mode -eq 'mailbox'){
  # ApplyVSyncSettings derives defaults; command-line values retain priority.
  $args+='--vulkan_allow_present_mode_immediate=false','--vulkan_allow_present_mode_mailbox=true','--vulkan_allow_present_mode_fifo_relaxed=false'
}
$meta=[ordered]@{label=$Label;backend=$Backend;mode=$Mode;cap=$Cap;baseline=$Baseline.IsPresent;args=$args;files=@(Get-ChildItem $run -File | Where-Object {$_.Extension -in '.exe','.dll'} | ForEach-Object {@{name=$_.Name;sha256=(Get-FileHash $_.FullName).Hash}})}
$meta | ConvertTo-Json -Depth 5 | Set-Content "$run/test.json" -Encoding UTF8
Wait-GameSlot
$guard=$null
$game=$null
try {
  $guard=[IO.File]::Open($lockPath,'Open','ReadWrite','Read')
  $reader=New-Object IO.StreamReader($guard,[Text.Encoding]::UTF8,$true,1024,$true)
  $state=$reader.ReadToEnd().Trim();$reader.Dispose()
  if($state -ne 'free'){throw 'Lock changed before claim'}
  if(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'"){throw 'Another game appeared'}
  if(Test-Path $priorityPath){if((Get-Content $priorityPath -Raw).Trim() -notin '096','task096'){throw 'Competing priority appeared'}}
  $guard.SetLength(0);$guard.Position=0
  $bytes=[Text.Encoding]::ASCII.GetBytes("task096 $(Get-Date -Format o)`r`n")
  $guard.Write($bytes,0,$bytes.Length);$guard.Flush()
  $game=Start-Process "$run/king_kong.exe" -WorkingDirectory $run -ArgumentList $args -WindowStyle Hidden -PassThru
  $runs+=@{run=$run;pid=$game.Id;label=$Label;time=(Get-Date -Format o)}
  ConvertTo-Json -InputObject ([object[]]$runs) -Depth 4 | Set-Content $countPath -Encoding UTF8
  Write-Output "Run $($runs.Count)/4: $run PID $($game.Id)"
  $deadline=(Get-Date).AddSeconds(240)
  $done=$false
  while(!$game.HasExited -and (Get-Date) -lt $deadline){
    $log=Read-SharedLog "$run/run.log"
    if($log.Contains('KK benchmark: complete,')){$done=$true;break}
    Start-Sleep -Seconds 2
  }
  if($done){
    Write-Output 'Benchmark completed';Start-Sleep -Seconds 2
    $game.Refresh()
    if($game.MainWindowHandle -ne [IntPtr]::Zero){[Capture096]::Save($game.MainWindowHandle,"$run/results.png")}
  }
  else {Write-Output "Benchmark did not complete; exited=$($game.HasExited)"}
} finally {
  if($game -and !$game.HasExited){
    [void]$game.CloseMainWindow()
    if(!$game.WaitForExit(10000)){Stop-Process -Id $game.Id -Force;$game.WaitForExit()}
  }
  if($guard){
    $guard.SetLength(0);$guard.Position=0
    $bytes=[Text.Encoding]::ASCII.GetBytes("free`r`n")
    $guard.Write($bytes,0,$bytes.Length);$guard.Flush();$guard.Dispose()
  }
}
Write-Output "Evidence: $run"
