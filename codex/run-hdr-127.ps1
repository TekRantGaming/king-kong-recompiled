param([ValidateSet('vulkan','d3d12')][string]$Backend='vulkan',
      [ValidateSet('fsr3','dlss','xess')][string]$Upscaler='fsr3', [switch]$Sdr, [switch]$Panel)
$ErrorActionPreference='Stop'
$taskRoot=(Resolve-Path "$PSScriptRoot/..").Path
. "$PSScriptRoot/game-slot.ps1"
. "$taskRoot/native-renderer/tests/run-storage.ps1"
if((Read-GameSlotState).StartsWith('user')) { throw 'USER LOCK: code-only work, no launch' }
if(!(Get-Process kkbg2 -ErrorAction SilentlyContinue)) { throw 'kkbg2 must be running' }
$countPath="$taskRoot/kk/out/hdr-sdk-127/game-runs.json"
function Flatten-RunRecords($node) {
  if($node -is [array]) { foreach($child in $node){Flatten-RunRecords $child} }
  elseif($node -and $node.PSObject.Properties['value']) { Flatten-RunRecords $node.value }
  elseif($node -and $node.PSObject.Properties['run']) { $node }
}
$runs=@()
if(Test-Path -LiteralPath $countPath) { $runs=@(Flatten-RunRecords (Get-Content -Raw -LiteralPath $countPath | ConvertFrom-Json)) }
# Per-run metadata also counts failed launches and survives old PowerShell
# JSON array wrappers. Never undercount the task's four-launch ceiling.
foreach($folder in Get-ChildItem -LiteralPath 'F:\KK-native-renderer\runs' -Directory -Filter 'task127-*') {
  $metadata=Join-Path $folder.FullName 'test.json'
  if(Test-Path -LiteralPath $metadata) {
    $candidate=Get-Content -Raw -LiteralPath $metadata | ConvertFrom-Json
    if($candidate.run -and $candidate.run -notin @($runs | ForEach-Object {$_.run})) { $runs+=$candidate }
  }
}
if($runs.Count -ge 4) { throw 'Task 127 four-run limit reached' }
$run="F:\KK-native-renderer\runs\task127-$Backend-$Upscaler-$(if($Sdr){'sdr'}else{'hdr'})-$(Get-Date -Format yyyyMMdd-HHmmss)"
$build="$taskRoot/kk/out/build/kk-dev-vulkan"
Initialize-RunBuild -Build $build -Run $run -ShaderPacks @('F:/KK-native-renderer/runs/task113-current-vulkan-20261015-151840/kkshaders-spirv.pack','F:/KK-native-renderer/runs/task113-current-vulkan-20261015-151840/kkshaders-dxil.pack') -Dxc 'F:/KK-native-renderer/downloads/dxc-v1.9.2609/dxc/bin/x64/dxcompiler.dll'
Copy-Item -LiteralPath 'F:\KK-native-renderer\testbuilds\beta5-test-1015-0739\KingKong\kkpipelines.list' -Destination $run
robocopy F:\KK-native-renderer\userdata "$run\userdata" /E /NFL /NDL /NJH /NJS /NP | Out-Null
if($LASTEXITCODE -ge 8){throw 'Private userdata copy failed'}
[IO.File]::WriteAllText("$run/king_kong.toml", "kk_settings_version = 3`nkk_render_quality = `"native`"`ngpu_plugin = `"native`"`n")
Get-ChildItem Env: | Where-Object {$_.Name -like 'KK_DEV_*' -or $_.Name -like 'REX_DEV_*'} | ForEach-Object {Remove-Item "Env:$($_.Name)"}
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'
$env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
$env:KK_DEV_AUTOSKIP='1'
$env:KK_DEV_SHOTS='15,30'
$env:KK_DEV_SHOTS_FROM='menu'
$env:KK_DEV_SHOTS_DIR=$run
if($Backend -eq 'd3d12' -and !$Sdr -and !$Panel) { $env:REX_DEV_HDR_CAPTURE="$run/hdr-swapchain.rgba16" }
if($Panel) {
  $env:KK_DEV_PANEL_TEST="$run/panel"
  $env:KK_DEV_PANEL_ONLY='present_hdr,present_hdr_peak,present_hdr_paper_white'
  $env:KK_DEV_PANEL_VERIFY='1'
  $env:KK_DEV_PANEL_START='8'
}
$arguments=@('--kk_startup_child=true','--kk_launcher=false',
 '--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
 "--user_data_root=$run\userdata","--game_data_root=$($taskRoot.Replace('\','/'))/kk/assets",
 "--cache_root=$run\cache","--log_file=$run\run.log","--native_backend=$Backend",
 "--config=$run/king_kong.toml",'--kk_renderer=native','--native_render_scale=1','--resolution_scale=1',
 "--native_upscaler=$Upscaler",'--native_upscaler_quality=quality','--native_upscaler_sharpness=0.2',
 '--present_effect=bilinear',"--present_hdr=$((!$Sdr).ToString().ToLower())",'--present_hdr_peak=1000','--present_hdr_paper_white=200',
 '--kk_original_look=false','--kk_frame_rate=60','--native_async_pipelines=true','--native_validation=true',
 '--kk_cheats=true','--kk_cheat_healing=true',
 '--kk_check_updates=false','--kk_online_count=false','--kk_discord_status=false','--kk_show_fps=false')
if($Panel) { $arguments+='--kk_start=chapter:0:8' } else { $arguments+='--kk_benchmark=true' }
$guard=Wait-GameSlot -Task '127' -RequireBackgroundHelper
$game=$null
try {
  $entry=[pscustomobject]@{run=$run;backend=$Backend;upscaler=$Upscaler;hdr=(!$Sdr);panel=$Panel.IsPresent;started=(Get-Date -Format o);args=$arguments}
  $runs+=$entry
  ConvertTo-Json -InputObject @($runs) -Depth 5 | Set-Content -LiteralPath $countPath -Encoding UTF8
  $entry | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$run/test.json" -Encoding UTF8
  $game=Start-Process -FilePath "$run/king_kong.exe" -WorkingDirectory $run -ArgumentList $arguments -PassThru -WindowStyle Hidden
  Write-Output "Started owned PID $($game.Id), evidence $run"
  $deadline=(Get-Date).AddSeconds(240)
  $complete=$false
  while(!$game.HasExited -and (Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    if(Test-Path -LiteralPath "$run/run.log") {
      $stream=[IO.File]::Open("$run/run.log",'Open','Read','ReadWrite')
      $reader=[IO.StreamReader]::new($stream)
      try {$log=$reader.ReadToEnd()} finally {$reader.Dispose()}
      if((!$Panel -and $log.Contains('KK benchmark: complete,')) -or
         ($Panel -and $log.Contains('KK panel test: done:'))) {$complete=$true;break}
    }
  }
  Write-Output "Run complete=$complete, panel=$Panel, process exited=$($game.HasExited)"
  if(!$complete) { throw 'Run did not complete' }
} finally {
  if($game -and !$game.HasExited) {
    [void]$game.CloseMainWindow()
    if(!$game.WaitForExit(10000)) {Stop-Process -Id $game.Id -Force;$game.WaitForExit()}
  }
  Release-GameSlot $guard
}
Assert-RunBuildUnchanged $run
Write-Output "Completed $run"
