param([ValidateSet('vulkan','d3d12')][string]$Backend='vulkan', [int]$Chapter=8,
 [ValidateSet('chapter','audio','launcher','notice')][string]$Mode='chapter', [switch]$DebugLayer)
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path "$PSScriptRoot/../..").Path
$lock='F:\KK-native-renderer\runs\codex-game.lock'
$priority='F:\KK-native-renderer\runs\codex-game.priority'
function Read-State {
 try { $state=(Get-Content -LiteralPath $lock -Raw).Trim() } catch [IO.IOException] { return 'busy' }
 if($state.StartsWith('user')) { throw 'USER LOCK: code-only work, no launch' }
 return $state
}
if(!(Test-Path -LiteralPath $lock)) { Set-Content -LiteralPath $lock 'free' }
$null=Read-State
$run="F:\KK-native-renderer\runs\task052-$Mode-$Backend-ch$Chapter-$(Get-Date -Format yyyyMMdd-HHmmss)"
New-Item -ItemType Directory -Path $run | Out-Null
$build="$workspace/kk/out/build/kk-dev-vulkan"
# Launcher runs use private copies because launcher shader preparation may write.
Get-ChildItem -LiteralPath $build -File | Where-Object { $_.Extension -in '.exe','.dll' } | Copy-Item -Destination $run
Copy-Item -LiteralPath "$build/glyphs" -Destination $run -Recurse
Copy-Item -LiteralPath 'F:\KK-native-renderer\cache\native-shaders\kkshaders-spirv.pack' -Destination $run
Copy-Item -LiteralPath 'F:\KK-native-renderer\runs\task025-20261013\sanity-d3d12-ch8-210057\kkshaders-dxil.pack' -Destination $run
Copy-Item -LiteralPath 'F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc\bin\x64\dxcompiler.dll' -Destination $run
robocopy F:\KK-native-renderer\userdata "$run\userdata" /E /NFL /NDL /NJH /NJS /NP | Out-Null
if($LASTEXITCODE -ge 8) { throw 'Private userdata copy failed' }
Get-ChildItem Env: | Where-Object { $_.Name -like 'KK_DEV_*' -or $_.Name -like 'REX_DEV_*' -or $_.Name -like 'VK_*' -or $_.Name -eq 'KK_STARTUP_PIPE' } | ForEach-Object { Remove-Item "Env:$($_.Name)" }
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
$env:KK_DEV_AUTOSKIP='1'; $env:KK_DEV_SCRIPT='0:SKIP/120'
$env:KK_DEV_SHOTS='30,120'; $env:KK_DEV_SHOTS_FROM='menu'; $env:KK_DEV_SHOTS_DIR=$run
$env:KK_AUTOPLAY='100000'
$arguments=@('--kk_startup_child=true','--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
 "--user_data_root=$run\userdata","--game_data_root=$($workspace.Replace('\','/'))/kk/assets",
 "--log_file=$run\run.log","--cache_root=$run\cache",'--kk_frame_rate=30',
 '--kk_renderer=native',"--native_backend=$Backend",'--native_render_scale=1280x720',
 '--native_upscaler=dlss','--native_upscaler_quality=quality','--native_upscaler_dlss_preset=latest',
 '--native_gi=off','--native_shadow_quality=original','--native_water_reflections=original','--native_volumetrics=off',
 '--ao_mode=0','--native_validation=true','--native_async_pipelines=false','--native_launcher_prewarm=false',
 '--kk_check_updates=false','--kk_online_count=false','--kk_online_asked=true','--kk_discord_status=false',
 '--kk_last_version=2.0.0-beta.3','--kk_launcher_music=false','--kk_launcher_sounds=false')
if($DebugLayer) { $arguments+='--d3d12_debug=true' }
if($Mode -in 'chapter','audio') { $arguments+="--kk_start=chapter:0:$Chapter",'--kk_launcher=false' }
else { $arguments+='--kk_launcher=true'; $env:KK_DEV_SHOTS=$null }
if($Mode -eq 'audio') {
 $arguments=@($arguments | Where-Object { $_ -notmatch '^--native_validation=|^--native_upscaler=' })
 $arguments+='--native_validation=false','--native_upscaler=off'
 $env:KK_DEV_PAGE_TRACE='1'; $env:KK_DEV_AUDIO_TRACE='1'
 $env:KK_DEV_SCRIPT='0:SKIP/10,12:START/0.12,78:START/0.12'
 $env:KK_DEV_PAUSE_INPUT='pad:down,pad:down,pad:down,pad:down,pad:a,pad:rb,pad:rb,pad:rb,pad:rb,pad:rb,shot:audio-edit,pad:left,pad:down,pad:left,pad:down,pad:left,pad:down,pad:left,pad:rb,pad:right,shot:subtitles-edit,pad:start'
 $env:KK_DEV_PAUSE_ACTIONS='80:options,84:optpick=0'
}
if($Mode -eq 'launcher') {
 $env:KK_DEV_DIAGNOSTICS='1'
 $env:KK_DEV_LAUNCHER_INPUT='pad:rb,pad:rb,pad:down,pad:a,shot:recommended,pad:rb,pad:rb,pad:rb,pad:rb,shot:audio,pad:rb,shot:subtitles,pad:rb,pad:rb,shot:about'
}
if($Mode -eq 'notice') {
 New-Item -ItemType Directory -Path "$run\userdata\renderer-recovery" -Force | Out-Null
 Set-Content -LiteralPath "$run\userdata\renderer-recovery\fallback.state" -Value "vulkan`nd3d12`n0`nVulkan failed to initialize (task 052 forced state). Using Direct3D 12 for safe startup.`n0" -Encoding ascii
 $env:KK_DEV_LAUNCHER_INPUT='shot:notice,pad:a,shot:continued'
}
if($Backend -eq 'vulkan' -and $Mode -ne 'audio') {
 @('khronos_validation.validate_sync = true','khronos_validation.gpuav_enable = true',
 'khronos_validation.report_flags = info,warn,error','khronos_validation.duplicate_message_limit = 20',
 'khronos_validation.debug_action = VK_DBG_LAYER_ACTION_LOG_MSG',"khronos_validation.log_filename = $run\vulkan-validation.log") | Set-Content "$run\vk_layer_settings.txt"
 $env:VK_LAYER_PATH='F:\KK-native-renderer\downloads\vulkan-sdk-latest\sdk\Bin'
 $env:VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation'; $env:VK_LAYER_SETTINGS_PATH=$run
}
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class Task052Window {
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint flags);
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[Task052Window]::SetProcessDPIAware() | Out-Null
function Capture-Own($game,$name) {
 $game.Refresh(); if(!$game.MainWindowHandle) { return }
 $r=New-Object Task052Window+RECT
 if(![Task052Window]::GetClientRect($game.MainWindowHandle,[ref]$r)) { return }
 $bmp=New-Object System.Drawing.Bitmap ($r.R-$r.L),($r.B-$r.T)
 $g=[System.Drawing.Graphics]::FromImage($bmp); $dc=$g.GetHdc()
 $ok=[Task052Window]::PrintWindow($game.MainWindowHandle,$dc,3)
 $g.ReleaseHdc($dc); $g.Dispose()
 if($ok) { $bmp.Save("$run\ui-$name.png",[System.Drawing.Imaging.ImageFormat]::Png) }
 $bmp.Dispose()
}
@{mode=$Mode;backend=$Backend;chapter=$Chapter;args=$arguments;exe_sha256=(Get-FileHash "$run\king_kong.exe").Hash;runtime_sha256=(Get-FileHash "$run\rexruntime.dll").Hash;native_sha256=(Get-FileHash "$run\rexgpu-native.dll").Hash;amd_sha256=(Get-FileHash "$run\amd_fidelityfx_vk.dll").Hash} | ConvertTo-Json -Depth 4 | Set-Content "$run\test.json"
$game=$null; $claimed=$false
try {
 while(!$claimed) {
  $state=Read-State
  $games=@(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'")
  $allowed=!(Test-Path $priority) -or (Get-Content $priority -Raw) -match '^task052\b'
  if($state -eq 'free' -and !$games.Count -and $allowed) {
   $guard=$null
   try {
    $guard=[IO.File]::Open($lock,'Open','ReadWrite','None')
    $reader=[IO.StreamReader]::new($guard,[Text.Encoding]::UTF8,$true,1024,$true)
    $current=$reader.ReadToEnd().Trim(); $reader.Dispose()
    if($current -eq 'free' -and !(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'")) {
     if(!(Get-Process kkbg2 -ErrorAction SilentlyContinue)) { throw 'kkbg2 required' }
     $guard.SetLength(0); $guard.Position=0; $bytes=[Text.Encoding]::ASCII.GetBytes("task052 $(Get-Date -Format o)"); $guard.Write($bytes,0,$bytes.Length); $guard.Flush(); $claimed=$true
    }
   } catch [IO.IOException] { } finally { if($guard) { $guard.Dispose() } }
  }
  if(!$claimed) {
   Write-Output "WAIT $state"
   $userGame=@($games | Where-Object { !$_.ExecutablePath -or ($_.ExecutablePath -notlike 'F:\KK-native-renderer\runs\*' -and $_.ExecutablePath -notlike 'F:\KK-native-renderer\wt\*') })
   Start-Sleep -Seconds $(if($userGame.Count){60}else{30})
  }
 }
 $game=Start-Process -FilePath "$run\king_kong.exe" -ArgumentList $arguments -WorkingDirectory $run -WindowStyle Hidden -PassThru -RedirectStandardOutput "$run\stdout.log" -RedirectStandardError "$run\stderr.log"
 Write-Output "RUN $run PID $($game.Id)"
 $timer=[Diagnostics.Stopwatch]::StartNew(); $shots=@{}
 while(!$game.HasExited -and $timer.Elapsed.TotalSeconds -lt $(if($Mode -in 'launcher','notice'){120}else{240})) {
  if(Test-Path "$run\run.log") {
   foreach($m in (Select-String "$run\run.log" -Pattern 'KK dev: shot ([a-zA-Z0-9_-]+)')) {
    $name=$m.Matches[0].Groups[1].Value
    if(!$shots.ContainsKey($name)) { Start-Sleep -Milliseconds 300; Capture-Own $game $name; $shots[$name]=$true }
   }
  }
  if(Test-Path "$run\shot_1200.png") { break }
  Start-Sleep -Milliseconds 500
 }
} finally {
 if($game -and !$game.HasExited) { $null=$game.CloseMainWindow(); if(!$game.WaitForExit(10000)) { Stop-Process -Id $game.Id; $game.WaitForExit() } }
 if($claimed -and (Read-State).StartsWith('task052 ')) { Set-Content -LiteralPath $lock 'free' -Encoding ascii }
 if($game) { @{pid=$game.Id;seconds=$timer.Elapsed.TotalSeconds;exit_code=$game.ExitCode;capture120=(Test-Path "$run\shot_1200.png")} | ConvertTo-Json | Set-Content "$run\result.json" }
}
Write-Output "DONE $run"
