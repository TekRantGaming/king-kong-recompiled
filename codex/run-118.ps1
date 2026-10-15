param(
 [Parameter(Mandatory)][string]$Label,
 [ValidateSet('vulkan','d3d12')][string]$Api='vulkan',
 [int]$Cap=60,
 [int]$Chapter=7,
 [double]$SkipAfter=0,
 [string]$PromptStyle='auto',
 [switch]$Disabled,
 [string]$MovieFolder='',
 [string]$Build='F:\KK-native-renderer\wt\ps2-movies\kk\out\task118\dev'
)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\game-slot.ps1"
$task='task118'
$ledger='F:\KK-native-renderer\runs\task118-launches.txt'
if((Read-GameSlotState).StartsWith('user')){throw 'USER LOCK: code-only work; no game launch or wait'}
if((Test-Path -LiteralPath $ledger) -and @(Get-Content -LiteralPath $ledger).Count -ge 4){throw 'Four-run limit reached'}
$run="F:\KK-native-renderer\runs\task118-$Label-"+(Get-Date -Format yyyyMMdd-HHmmss)
New-Item -ItemType Directory -Path $run | Out-Null
$base='F:\KK-native-renderer\testbuilds\beta6-test3-1015-1729\KingKong'
& robocopy $base $run /E /XF 'benchmark-route.kkbr' 'TEST-CHECKLIST.txt' /NFL /NDL /NJH /NJS | Out-Null
if($LASTEXITCODE -gt 7){throw 'Package copy failed'}
Copy-Item -LiteralPath "$Build\king_kong.exe","$Build\rexgpu-native.dll","$Build\rexruntime.dll" -Destination $run
$packs='F:\KK-native-renderer\runs\task113-current-vulkan-20261015-151840'
Copy-Item -LiteralPath "$packs\kkshaders-spirv.pack","$packs\kkshaders-dxil.pack" -Destination $run
& robocopy 'F:\KK-native-renderer\userdata' "$run\userdata" /E /NFL /NDL /NJH /NJS | Out-Null
if($LASTEXITCODE -gt 7){throw 'Private userdata copy failed'}
$gameArgs=@('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
 '--kk_renderer=native',"--native_backend=$Api","--kk_frame_rate=$Cap",'--vsync=false',
 '--kk_launcher=false',"--kk_start=chapter:0:$Chapter",'--kk_startup_child=true',
 "--user_data_root=$run\userdata",'--game_data_root=F:/KK-native-renderer/wt/ps2-movies/kk/assets',
 "--cache_root=$run\cache","--log_file=$run\run.log","--config=$run\king_kong.toml",
 '--kk_check_updates=false','--kk_online_count=false','--kk_discord_status=false',
 '--video_mode_width=1280','--video_mode_height=720','--draw_resolution_scale_x=1','--draw_resolution_scale_y=1',
 '--kk_render_quality=native','--native_upscaler=off','--native_gi=off','--kk_original_look=false',
 '--native_shadow_quality=original','--native_water_reflections=original','--native_volumetrics=off',
 "--kk_button_prompts=$PromptStyle",('--kk_ps2_cutscenes='+(-not $Disabled).ToString().ToLowerInvariant()))
if($MovieFolder){$gameArgs+="--kk_ps2_movie_folder=$MovieFolder"}
$gameArgs | Set-Content -LiteralPath "$run\arguments.txt"
Get-ChildItem Env: | Where-Object { $_.Name -like 'KK_DEV_*' -or $_.Name -eq 'KK_AUTOPLAY' } | ForEach-Object { [Environment]::SetEnvironmentVariable($_.Name,$null,'Process') }
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
$env:KK_DEV_START_END='3'; $env:KK_DEV_VRAM_TOUR='1'
if($SkipAfter -gt 0){$env:KK_DEV_PS2_SKIP_AFTER=$SkipAfter.ToString([Globalization.CultureInfo]::InvariantCulture)}
$shots="F:\KK-native-renderer\documentary\ps2-movies\$Label"
New-Item -ItemType Directory -Path $shots -Force | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Movie118Shot {
 [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr v);
 [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr w,IntPtr dc,uint f);
 [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr w,out Rect r);
 [StructLayout(LayoutKind.Sequential)] struct Rect {public int l,t,r,b;}
 public static void Capture(IntPtr w,string path) {
  var previous=SetThreadDpiAwarenessContext(new IntPtr(-4));
  try {
   Rect r;if(!GetWindowRect(w,out r)||r.r<=r.l||r.b<=r.t)return;
   using(var b=new System.Drawing.Bitmap(r.r-r.l,r.b-r.t))
   using(var g=System.Drawing.Graphics.FromImage(b)) {
    var dc=g.GetHdc();bool ok;
    try {ok=PrintWindow(w,dc,2);} finally {g.ReleaseHdc(dc);}
    if(ok)b.Save(path,System.Drawing.Imaging.ImageFormat.Png);
   }
  } finally {SetThreadDpiAwarenessContext(previous);}
 }
}
'@
function Read-OwnLog {
 if(!(Test-Path -LiteralPath "$run\run.log")){return ''}
 $stream=[IO.File]::Open("$run\run.log",'Open','Read','ReadWrite')
 $reader=[IO.StreamReader]::new($stream)
 try{return $reader.ReadToEnd()}finally{$reader.Dispose()}
}
$guard=Wait-GameSlot -Task $task -RequireBackgroundHelper
$owned=@()
try {
 if(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'"){throw 'A game appeared after slot claim'}
 if((Test-Path -LiteralPath $ledger) -and @(Get-Content -LiteralPath $ledger).Count -ge 4){throw 'Four-run limit reached'}
 $p=Start-Process -FilePath "$run\king_kong.exe" -ArgumentList $gameArgs -WorkingDirectory $run -WindowStyle Hidden -PassThru
 $owned+=$p.Id
 "$Label $Api cap=$Cap chapter=$Chapter skip=$SkipAfter pid=$($p.Id) $run" | Add-Content -LiteralPath $ledger
 Write-Output "LAUNCHED $($p.Id) $run"
 $deadline=(Get-Date).AddMinutes(3)
 $movieAt=$null;$loadedAt=$null;$seen=@{};$last=''
 while((Get-Date)-lt $deadline) {
  $games=@(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'")
  if($games | Where-Object {$_.ProcessId -notin $owned}){throw 'Another game appeared; stopping only the owned test'}
  if($p.HasExited){throw "Game exited early: $run"}
  $log=Read-OwnLog
  if(!$movieAt -and $log -match 'PS2 movie: clock started'){$movieAt=Get-Date}
  if(!$loadedAt -and ($log -match 'PS2 movie: original level loader returned' -or
      (($Disabled -or $MovieFolder) -and $log -match 'KK dev jump: asking for the next chapter'))){$loadedAt=Get-Date}
  $markers=@([regex]::Matches($log,'(?m)^.*(?:PS2 movie:|KK dev jump: asking for the next chapter).*$'))
  if($markers.Count -and $markers[-1].Value -ne $last){$last=$markers[-1].Value;Write-Output $last}
  $p.Refresh()
  if($p.MainWindowHandle -ne 0) {
   if(!$seen.ContainsKey('initial') -and $log -match 'the start request.s level is loaded') {
    [Movie118Shot]::Capture($p.MainWindowHandle,"$shots\initial.png");$seen['initial']=$true
   }
   if($movieAt) {
    foreach($second in @(1,5,9)) {
     if(!$seen.ContainsKey("movie-$second") -and !$loadedAt -and ((Get-Date)-$movieAt).TotalSeconds -ge $second) {
      [Movie118Shot]::Capture($p.MainWindowHandle,"$shots\movie-$second.png");$seen["movie-$second"]=$true
     }
    }
   }
   if($loadedAt) {
    foreach($second in @(3,10)) {
     if(!$seen.ContainsKey("next-$second") -and ((Get-Date)-$loadedAt).TotalSeconds -ge $second) {
      [Movie118Shot]::Capture($p.MainWindowHandle,"$shots\next-$second.png");$seen["next-$second"]=$true
     }
    }
   }
  }
  if($loadedAt -and ((Get-Date)-$loadedAt).TotalSeconds -gt 12){break}
  Start-Sleep -Milliseconds 250
 }
 if(!$loadedAt){throw "Transition did not finish within three minutes: $run"}
 [pscustomobject]@{Run=$run;Pid=$p.Id;Api=$Api;Cap=$Cap;MovieStarted=[bool]$movieAt;Loaded=[bool]$loadedAt;Shots=$seen.Keys;SkipAfter=$SkipAfter} |
  ConvertTo-Json | Set-Content -LiteralPath "$run\test-result.json"
} finally {
 foreach($own in $owned){$proc=Get-Process -Id $own -ErrorAction SilentlyContinue;if($proc){Stop-Process -Id $own;Wait-Process -Id $own -ErrorAction SilentlyContinue}}
 Release-GameSlot $guard
}
