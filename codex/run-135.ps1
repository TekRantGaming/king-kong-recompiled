param([ValidateSet('original','ending','gallery','movie')][string]$Mode='original')
$ErrorActionPreference='Stop'
. "$PSScriptRoot/game-slot.ps1"
$workspace=(Resolve-Path "$PSScriptRoot/..").Path
$build="$workspace/kk/out/build/kk-beta"
$ledger='F:\KK-native-renderer\runs\task135-launches.txt'
if((Read-GameSlotState).StartsWith('user')){throw 'USER LOCK: code-only work; no launch or wait'}
if((Test-Path $ledger) -and @(Get-Content $ledger).Count -ge 4){throw 'Four-run limit reached'}
$run='F:\KK-native-renderer\runs\task135-'+$Mode+'-'+(Get-Date -Format yyyyMMdd-HHmmss)
New-Item -ItemType Directory -Path $run | Out-Null
Get-ChildItem -LiteralPath $build -File | Where-Object {$_.Extension -in '.exe','.dll','.pack'} | Copy-Item -Destination $run
& robocopy "$build/glyphs" "$run/glyphs" /E /NFL /NDL /NJH /NJS | Out-Null
if($LASTEXITCODE -gt 7){throw 'Glyph copy failed'}
$packs='F:\KK-native-renderer\runs\task113-current-vulkan-20261015-151840'
Copy-Item -LiteralPath "$packs/kkshaders-spirv.pack","$packs/kkshaders-dxil.pack" -Destination $run
& robocopy 'F:\KK-native-renderer\userdata' "$run/userdata" /E /NFL /NDL /NJH /NJS | Out-Null
if($LASTEXITCODE -gt 7){throw 'Private userdata copy failed'}
Get-ChildItem Env: | Where-Object {$_.Name -like 'KK_DEV_*' -or $_.Name -eq 'KK_AUTOPLAY'} |
 ForEach-Object {[Environment]::SetEnvironmentVariable($_.Name,$null,'Process')}
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0';$env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
$env:KK_DEV_EXTRAS_TRACE='1'
$env:KK_AUTOPLAY='99999'
$env:KK_DEV_LAUNCHER_INPUT_FILE="$run/launcher-input.txt"
New-Item -ItemType File -Path $env:KK_DEV_LAUNCHER_INPUT_FILE -Force | Out-Null
$cached='F:\KK-native-renderer\runs\task135-ending-20261016-005707\cache\native-shaders'
if(Test-Path -LiteralPath $cached) {
 New-Item -ItemType Directory -Path "$run/cache/native-shaders" -Force | Out-Null
 Get-ChildItem -LiteralPath $cached -File | Copy-Item -Destination "$run/cache/native-shaders"
}
$shots='E:\KK-documentry\documentary\launcher-extras\'+(Split-Path $run -Leaf)
New-Item -ItemType Directory -Path $shots -Force | Out-Null
$gameArgs=@('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
 '--kk_renderer=native','--native_backend=vulkan','--kk_frame_rate=60','--kk_startup_child=true',
 "--user_data_root=$run/userdata",('--game_data_root='+$workspace.Replace('\','/')+'/kk/assets'),
 "--cache_root=$run/cache","--log_file=$run/run.log","--config=$run/king_kong.toml",
 '--kk_check_updates=false','--kk_online_count=false','--kk_discord_status=false','--native_launcher_prewarm=false',
 '--kk_last_version=2.0.0-beta.5',
 '--native_pipeline_list=false','--native_pipeline_cache=false','--native_vk_pipeline_cache=false',
 '--kk_render_quality=native','--native_upscaler=off','--native_gi=off','--kk_original_look=false',
 '--kk_ps2_cutscenes=false','--kk_skip_intros=true')
if($Mode -eq 'movie'){$gameArgs=$gameArgs -replace '^--native_backend=vulkan$','--native_backend=d3d12'}
if($Mode -eq 'original') {
 $gameArgs+='--kk_launcher=false'
 $env:KK_DEV_AUTOSKIP='1';$env:KK_DEV_SCRIPT='1:A,6:A'
 $env:KK_DEV_PAGE_TRACE='1'
} else {
 # The private test-copy unlock changes are performed by prepare-135.py, never on source userdata.
 & 'C:\Users\TekRa\AppData\Local\Programs\Python\Python312\python.exe' -I "$workspace/codex/prepare-135.py" "$run/userdata"
 if($LASTEXITCODE){throw 'Test save preparation failed'}
 $gameArgs+=@('--kk_launcher=true','--kk_launcher_open=extras')
 $entry=if($Mode -eq 'ending'){15}elseif($Mode -eq 'gallery'){3}else{1}
 $steps=@('extras-save:0')+(@('pad:down')*$entry)+@('shot:bonus-locked','pad:a','extras-save:1','shot:bonus-unlocked','pad:a')
 if($Mode -eq 'movie') {
  $steps=@('extras-save:1')
  foreach($page in @('Display','Graphics','Audio','Controls','Gameplay','Advanced')){$steps+=@("page:$page","shot:category-$page")}
  $steps+='page:Extras'
  foreach($language in @(3,5,4,6,1)){$steps+=@("language:$language","shot:extras-language-$language")}
  $steps+=@('pad:down','shot:trailer','pad:a')
 }
 $env:KK_DEV_LAUNCHER_INPUT=$steps -join ','
 $env:KK_DEV_AUTOSKIP='1'
 if($Mode -eq 'movie'){$env:KK_DEV_SCRIPT='8:SKIP'}
 elseif($Mode -eq 'gallery'){$env:KK_DEV_SCRIPT='8:B'}
 else {$env:KK_DEV_SCRIPT='1:SKIP';$env:KK_DEV_PAUSE_ACTIONS='22:open,25:quit'}
}
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Extras135Shot {
 [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr w,IntPtr dc,uint f);
 [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr w,out Rect r);
 [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr v);
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
$guard=Wait-GameSlot -Task task135 -RequireBackgroundHelper
$owned=@()
try {
 if(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'"){throw 'A game appeared after slot claim'}
 $p=Start-Process -FilePath "$run/king_kong.exe" -ArgumentList $gameArgs -WorkingDirectory $run -WindowStyle Hidden -PassThru
 $owned+=$p.Id
 "$Mode pid=$($p.Id) $run" | Add-Content -LiteralPath $ledger
 Write-Output "LAUNCHED $($p.Id) $run"
 $deadline=(Get-Date).AddSeconds(140);$start=Get-Date;$seen=@{};$loaded=$null;$returned=$null
 while((Get-Date)-lt $deadline) {
  $allGames=@(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'")
  if($allGames | Where-Object {$_.ExecutablePath -ne "$run\king_kong.exe"}){throw 'Another game appeared; stopping only our private test'}
  $processes=@($allGames)
  foreach($proc in $processes) {
   if($proc.ProcessId -notin $owned) {
    if($proc.ParentProcessId -notin $owned){throw 'Unowned process in test folder'}
    $owned+=$proc.ProcessId
   }
  }
  $active=@($owned | ForEach-Object {Get-Process -Id $_ -ErrorAction SilentlyContinue})
  if(!$active.Count){break}
  $log=''
  Get-ChildItem -LiteralPath $run -Filter 'run*.log' | ForEach-Object {
   $stream=[IO.File]::Open($_.FullName,'Open','Read','ReadWrite');$reader=[IO.StreamReader]::new($stream)
   try{$log+=$reader.ReadToEnd()}finally{$reader.Dispose()}
  }
  if(!$loaded -and $log -match "the start request.s level is loaded|KK extras: playing movie"){$loaded=Get-Date}
  foreach($proc in $active) {
   $proc.Refresh()
   if($proc.MainWindowHandle -eq 0){continue}
   foreach($m in [regex]::Matches($log,'KK dev: shot ([a-zA-Z0-9_-]+)')) {
    $name=$m.Groups[1].Value
    if(!$seen.ContainsKey($name)) {
     [Extras135Shot]::Capture($proc.MainWindowHandle,"$shots/$name.png");$seen[$name]=$true
     if($name -eq 'returned-extras'){$returned=Get-Date}
    }
   }
   foreach($s in @(15,30,45,60,80,110)) {
    if(!$seen.ContainsKey("$s") -and ((Get-Date)-$start).TotalSeconds -ge $s) {
     [Extras135Shot]::Capture($proc.MainWindowHandle,"$shots/frame-$s.png");$seen["$s"]=$true
    }
   }
   if($loaded) {foreach($s in @(1,4,7,12,20)) {
    $key="content-$s"
    if(!$seen.ContainsKey($key) -and ((Get-Date)-$loaded).TotalSeconds -ge $s) {
     [Extras135Shot]::Capture($proc.MainWindowHandle,"$shots/$key.png");$seen[$key]=$true
    }
   }}
  }
  if($returned -and ((Get-Date)-$returned).TotalSeconds -gt 3){break}
  Start-Sleep -Milliseconds 250
 }
 Write-Output "RUN $run"
} finally {
 foreach($own in $owned) {if(Get-CimInstance Win32_Process -Filter "ProcessId=$own" | Where-Object {$_.ExecutablePath -eq "$run\king_kong.exe"}){Stop-Process -Id $own;Wait-Process -Id $own -ErrorAction SilentlyContinue}}
 Release-GameSlot $guard
}
