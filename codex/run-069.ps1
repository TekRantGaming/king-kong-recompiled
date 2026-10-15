param([ValidateSet('both','vulkan','d3d12')][string]$Backend='both')
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path "$PSScriptRoot/..").Path
$lockPath='F:\KK-native-renderer\runs\codex-game.lock'
$priorityPath='F:\KK-native-renderer\runs\codex-game.priority'
$taskName='task069'
function Wait-GameSlot {
  while ($true) {
    if (!(Test-Path -LiteralPath $lockPath)) { Set-Content -LiteralPath $lockPath 'free' }
    $state=(Get-Content -LiteralPath $lockPath -Raw).Trim()
    if ($state.StartsWith('user')) { throw 'USER LOCK: no game launch; report code-only results.' }
    $priority=if (Test-Path $priorityPath) { (Get-Content $priorityPath -Raw).Trim() } else { '' }
    $games=@(Get-CimInstance Win32_Process -Filter "name = 'king_kong.exe'")
    $own=@($games | Where-Object { !$_.ExecutablePath -or ($_.ExecutablePath -notlike 'F:\KK-native-renderer\runs\*' -and $_.ExecutablePath -notlike 'F:\KK-native-renderer\wt\*') })
    if ($state -eq 'free' -and !$games.Count -and (!$priority -or $priority -eq $taskName -or $priority -eq '069')) { return }
    Write-Output "Waiting: lock=$state priority=$priority games=$($games.Count)"
    Start-Sleep -Seconds $(if ($own.Count) {60} else {30})
  }
}
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class UiPaint069 {
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out Rect r);
 [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hwnd,uint msg,IntPtr wp,IntPtr lp);
 [StructLayout(LayoutKind.Sequential)] public struct Rect { public int l,t,r,b; }
 public static void Shot(IntPtr hwnd,string path) {
  Rect r; if(!GetWindowRect(hwnd,out r)) throw new Exception("own-window rectangle failed");
  using(var image=new System.Drawing.Bitmap(r.r-r.l,r.b-r.t))
  using(var g=System.Drawing.Graphics.FromImage(image)) {
   var dc=g.GetHdc(); bool ok; try { ok=PrintWindow(hwnd,dc,2); } finally { g.ReleaseHdc(dc); }
   if(!ok) throw new Exception("own-window PrintWindow failed"); image.Save(path);
  }
 }
}
'@
$documentary=Join-Path $workspace 'documentary/uipaint'
New-Item -ItemType Directory -Path $documentary -Force | Out-Null
$campaign='F:\KK-native-renderer\runs\task069-'+(Get-Date -Format yyyyMMdd-HHmmss)
New-Item -ItemType Directory -Path $campaign | Out-Null
$apis=if($Backend -eq 'both') { @('vulkan','d3d12') } else { @($Backend) }
foreach ($api in $apis) {
  Wait-GameSlot
  $run=Join-Path $campaign $api
  New-Item -ItemType Directory -Path $run | Out-Null
  $build=Join-Path $workspace 'kk/out/build/kk-dev-vulkan'
  Get-ChildItem -LiteralPath $build -File | Where-Object { $_.Extension -in '.exe','.dll' } | Copy-Item -Destination $run
  Copy-Item F:/KK-native-renderer/sdk-vulkan-upscalers-037/win-amd64/bin/rexruntime.dll $run
  Copy-Item -LiteralPath "$build/glyphs" -Destination $run -Recurse
  Copy-Item F:/KK-native-renderer/cache/native-shaders/kkshaders-spirv.pack $run
  Copy-Item F:/KK-native-renderer/runs/task064-20261015-002710/cache/native-shaders/kkshaders-dxil.pack $run
  Copy-Item F:/KK-native-renderer/downloads/dxc-v1.9.2609/dxc/bin/x64/dxcompiler.dll $run
  robocopy F:\KK-native-renderer\userdata "$run\userdata" /E /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) { throw 'Private user data copy failed' }
  Get-ChildItem Env: | Where-Object { $_.Name -like 'KK_DEV_*' -or $_.Name -like 'REX_DEV_*' -or $_.Name -like 'VK_*' } | ForEach-Object { Remove-Item "Env:$($_.Name)" }
  $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
  $env:KK_DEV_AUTOSKIP='1'; $env:KK_DEV_SCRIPT='0:SKIP/72'; $env:KK_DEV_UIPAINT_TEST='1'
  $env:KK_DEV_PAUSE_ACTIONS='54:open,58:resume'
  $env:KK_DEV_SHOTS='10,22,34,46,50,56,59,63,68'
  $env:KK_DEV_SHOTS_FROM='menu'; $env:KK_DEV_SHOTS_DIR=$run
  $args=@('--kk_startup_child=true','--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
    '--kk_start=chapter:0:8','--kk_launcher=false','--kk_cheats=true','--kk_cheat_healing=true',
    "--user_data_root=$run\userdata","--game_data_root=$($workspace.Replace('\','/'))/kk/assets","--log_file=$run\run.log","--cache_root=$run\cache",
    "--native_backend=$api",'--native_upscaler=off','--native_render_scale=1280x720','--kk_original_look=true',
    '--kk_original_look_keep=kk_frame_rate,render_scale,native_render_scale,native_upscaler,ao_mode,ao_strength',
    '--kk_frame_rate=30','--kk_show_fps=false','--ao_mode=0','--ui_paint_on_demand=false','--ui_paint_trace=true',
    '--native_validation=false','--native_async_pipelines=false')
  Wait-GameSlot
  if (!(Get-Process kkbg2 -ErrorAction SilentlyContinue)) { throw 'kkbg2 must already be running' }
  # Claim under exclusive file access; recheck processes and priority afterwards.
  $claim=[IO.File]::Open($lockPath,'Open','ReadWrite','None')
  try {
    $reader=New-Object IO.StreamReader($claim,[Text.Encoding]::UTF8,$true,1024,$true)
    $state=$reader.ReadToEnd().Trim(); $reader.Dispose()
    if ($state -ne 'free') { throw 'Lock changed before claim' }
    $claim.SetLength(0); $claim.Position=0
    $bytes=[Text.Encoding]::UTF8.GetBytes("task069 $(Get-Date -Format o)")
    $claim.Write($bytes,0,$bytes.Length)
  } finally { $claim.Dispose() }
  $game=$null
  try {
    if (Get-CimInstance Win32_Process -Filter "name = 'king_kong.exe'") { throw 'Another game appeared before launch' }
    if (Test-Path $priorityPath) {
      $priority=(Get-Content $priorityPath -Raw).Trim()
      if ($priority -notin 'task069','069') { throw "Priority changed: $priority" }
    }
    @{api=$api;args=$args;exe=(Get-FileHash "$run/king_kong.exe").Hash;runtime=(Get-FileHash "$run/rexruntime.dll").Hash;renderer=(Get-FileHash "$run/rexgpu-native.dll").Hash} | ConvertTo-Json | Set-Content "$run/test.json"
    $game=Start-Process -FilePath "$run/king_kong.exe" -WorkingDirectory $run -ArgumentList $args -WindowStyle Hidden -PassThru -RedirectStandardOutput "$run/stdout.log" -RedirectStandardError "$run/stderr.log"
    Write-Output "RUN $api $run PID $($game.Id)"
    $wall=[Diagnostics.Stopwatch]::StartNew(); $level=$null; $captures=@{}; $opened=$false; $closed=$false
    while (!$game.HasExited -and $wall.Elapsed.TotalSeconds -lt 240) {
      $log=''
      if (Test-Path "$run/run.log") {
        try {
          $stream=[IO.File]::Open("$run/run.log",'Open','Read',([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
          try { $reader=New-Object IO.StreamReader($stream); $log=$reader.ReadToEnd() }
          finally { if($reader){$reader.Dispose()}else{$stream.Dispose()} }
        } catch [IO.IOException] { } # A logger rotation is transient, not a failed game run.
      }
      if (!$level -and $log -match 'UI repaint test phase 0 at') { $level=[Diagnostics.Stopwatch]::StartNew() }
      if ($level) {
        $t=$level.Elapsed.TotalSeconds
        $game.Refresh(); $hwnd=$game.MainWindowHandle
        if ($hwnd -ne [IntPtr]::Zero) {
          if ($t -ge 60 -and !$opened) { [void][UiPaint069]::PostMessageW($hwnd,0x100,[IntPtr]0x77,[IntPtr]0); [void][UiPaint069]::PostMessageW($hwnd,0x101,[IntPtr]0x77,[IntPtr]0); $opened=$true }
          if ($t -ge 66 -and !$closed) { [void][UiPaint069]::PostMessageW($hwnd,0x100,[IntPtr]0x77,[IntPtr]0); [void][UiPaint069]::PostMessageW($hwnd,0x101,[IntPtr]0x77,[IntPtr]0); $closed=$true }
          foreach ($at in @(10,22,34,46,50,56,59,63,68)) {
            if ($t -ge $at -and !$captures.ContainsKey($at)) {
              [UiPaint069]::Shot($hwnd,(Join-Path $documentary "$api-$at.png")); $captures[$at]=$true
            }
          }
        }
        if ($t -ge 72) { break }
      }
      Start-Sleep -Milliseconds 250
    }
    @{seconds=$wall.Elapsed.TotalSeconds;level_seconds=$(if($level){$level.Elapsed.TotalSeconds}else{0});captures=@($captures.Keys);f8_open_sent=$opened;f8_close_sent=$closed;exited=$game.HasExited} | ConvertTo-Json | Set-Content "$run/result.json"
  } finally {
    if ($game -and !$game.HasExited) { [void]$game.CloseMainWindow(); if (!$game.WaitForExit(10000)) { Stop-Process -Id $game.Id; $game.WaitForExit() } }
    if ((Get-Content $lockPath -Raw).StartsWith('task069 ')) { Set-Content $lockPath 'free' }
  }
  Write-Output "DONE $api $run"
}
Write-Output "CAMPAIGN $campaign"
