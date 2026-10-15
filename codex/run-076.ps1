param()
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path "$PSScriptRoot/..").Path
$lockPath='F:\KK-native-renderer\runs\codex-game.lock'
$priorityPath='F:\KK-native-renderer\runs\codex-game.priority'
$campaign='F:\KK-native-renderer\runs\task076-'+(Get-Date -Format yyyyMMdd-HHmmss)
New-Item -ItemType Directory -Path $campaign | Out-Null
Set-Content "$workspace\out\task076\campaign.txt" $campaign
function Wait-Slot {
 while ($true) {
  if (!(Test-Path $lockPath)) { Set-Content $lockPath 'free' }
  $owner=(Get-Content $lockPath -Raw).Trim()
  if ($owner.StartsWith('user')) { throw 'USER LOCK: no launches permitted; report code-only results' }
  $games=@(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'")
  $priority=if(Test-Path $priorityPath){(Get-Content $priorityPath -Raw).Trim()}else{''}
  if ($owner -eq 'free' -and !$games.Count -and (!$priority -or $priority -in 'task076','076')) { return }
  Write-Output "WAIT lock=$owner priority=$priority games=$($games.Count)"
  Start-Sleep -Seconds 30
 }
}
function Read-Live($path) {
 if (!(Test-Path $path)) {return ''}
 try {
  $s=[IO.File]::Open($path,'Open','Read',([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
  try {$r=[IO.StreamReader]::new($s); return $r.ReadToEnd()}finally{if($r){$r.Dispose()}else{$s.Dispose()}}
 } catch [IO.IOException] {return ''}
}
$build="$workspace\kk\out\build\kk-beta"
foreach ($phase in @('launcher','vulkan','d3d12')) {
 Wait-Slot
 $dir=Join-Path $campaign $phase
 New-Item -ItemType Directory -Path $dir | Out-Null
 Get-ChildItem $build -File | Where-Object {$_.Extension -eq '.dll'} | Copy-Item -Destination $dir
 Copy-Item "$build\king_kong.exe" $dir
 Copy-Item "$build\glyphs" $dir -Recurse
 Copy-Item 'F:\KK-native-renderer\cache\native-shaders\kkshaders-spirv.pack' $dir
 Copy-Item 'F:\KK-native-renderer\runs\task064-20261015-002710\cache\native-shaders\kkshaders-dxil.pack' $dir
 Copy-Item 'F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc\bin\x64\dxcompiler.dll' $dir
 robocopy 'F:\KK-native-renderer\userdata' "$dir\userdata" /E /NFL /NDL /NJH /NJS /NP | Out-Null
 if ($LASTEXITCODE -ge 8) {throw 'Private userdata copy failed'}
 if(Get-ChildItem $dir -Filter '*xenos*'){throw 'Xenos staged'}
 @('kk_last_version = "2.0.0-beta.4"','kk_settings_version = 2') | Set-Content "$dir\king_kong.toml"
 Get-ChildItem Env: | Where-Object {$_.Name -like 'KK_DEV_*' -or $_.Name -like 'REX_DEV_*' -or $_.Name -like 'VK_*' -or $_.Name -eq 'KK_STARTUP_PIPE'} | ForEach-Object {Remove-Item "Env:$($_.Name)"}
 $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
 $env:KK_AUTOPLAY='100000'
 $api=if($phase -eq 'd3d12'){'d3d12'}else{'vulkan'}
 $launcher=($phase -eq 'launcher').ToString().ToLower()
 $args=@('--kk_startup_child=true','--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
 "--kk_launcher=$launcher",'--kk_start=chapter:0:8',"--user_data_root=$dir\userdata", "--game_data_root=$($workspace.Replace('\','/'))/kk/assets",
 "--log_file=$dir\run.log","--cache_root=$dir\cache","--native_backend=$api",'--native_upscaler=dlss','--native_upscaler_quality=quality',
 '--native_render_scale=1280x720','--kk_original_look=true','--kk_original_look_keep=kk_frame_rate,native_render_scale,native_upscaler,native_upscaler_quality',
 '--kk_frame_rate=0','--ao_mode=0','--native_gi=off','--native_shadow_quality=original','--native_water_reflections=original','--native_volumetrics=off',
 '--ui_paint_trace=true','--native_diagnostics=false','--kk_check_updates=false','--kk_online_count=false','--kk_discord_status=false',
 '--native_launcher_prewarm=false','--native_validation=false','--native_async_pipelines=false','--kk_launcher_music=false','--kk_launcher_sounds=false',
 '--bind_console=F9')
 Wait-Slot
 if (!(Get-Process kkbg2 -ErrorAction SilentlyContinue)) {throw 'kkbg2 required'}
 $claim=[IO.File]::Open($lockPath,'Open','ReadWrite','None')
 try {
  $r=[IO.StreamReader]::new($claim,[Text.Encoding]::UTF8,$true,1024,$true); $state=$r.ReadToEnd().Trim();$r.Dispose()
  if($state -ne 'free'){throw 'Lock changed'}
  $claim.SetLength(0);$claim.Position=0;$bytes=[Text.Encoding]::UTF8.GetBytes("task076 $(Get-Date -Format o)");$claim.Write($bytes,0,$bytes.Length)
 }finally{$claim.Dispose()}
 $game=$null
 try {
  if (Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'") {throw 'Another game appeared'}
  if ((Test-Path $priorityPath) -and (Get-Content $priorityPath -Raw).Trim() -notin 'task076','076') {throw 'Priority changed'}
  @{phase=$phase;args=$args;exe=(Get-FileHash "$dir\king_kong.exe").Hash;runtime=(Get-FileHash "$dir\rexruntime.dll").Hash;renderer=(Get-FileHash "$dir\rexgpu-native.dll").Hash} | ConvertTo-Json | Set-Content "$dir\test.json"
  $game=Start-Process "$dir\king_kong.exe" -WorkingDirectory $dir -ArgumentList $args -WindowStyle Hidden -PassThru -RedirectStandardOutput "$dir\stdout.log" -RedirectStandardError "$dir\stderr.log"
  @{phase=$phase;pid=$game.Id;directory=$dir} | ConvertTo-Json | Set-Content "$workspace\out\task076\active.json"
  Write-Output "RUN $phase PID=$($game.Id) $dir"
  $wall=[Diagnostics.Stopwatch]::StartNew()
  while (!$game.HasExited -and $wall.Elapsed.TotalSeconds -lt 240 -and !(Test-Path "$dir\phase.done")) {
   if ((Get-Content $lockPath -Raw).StartsWith('user')) {throw 'User lock; ending only our game'}
   Start-Sleep -Milliseconds 300
  }
  @{seconds=$wall.Elapsed.TotalSeconds;exited=$game.HasExited;phase_completed=(Test-Path "$dir\phase.done");notes=$(if(Test-Path "$dir\phase.done"){Get-Content "$dir\phase.done" -Raw}else{'Timed out or exited; inspect log'})} | ConvertTo-Json | Set-Content "$dir\result.json"
 }finally{
  if($game -and !$game.HasExited){[void]$game.CloseMainWindow();if(!$game.WaitForExit(10000)){Stop-Process -Id $game.Id;$game.WaitForExit()}}
  if((Get-Content $lockPath -Raw).StartsWith('task076 ')){Set-Content $lockPath 'free'}
 }
}
Write-Output "CAMPAIGN $campaign"
