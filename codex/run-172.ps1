param(
 [ValidateSet('vulkan','d3d12')][string]$Api='vulkan',
 # launcher: the launcher's Run benchmark (Quick run), as players start it. cmd: king_kong.exe --kk_benchmark=true.
 [ValidateSet('launcher','cmd')][string]$Mode='launcher',
 [string]$Build='kk-dev-vulkan',
 [string]$Label=''
)
# Task 172 game runner (other-programs GPU check): one windowed, private 1-pass benchmark per call.
# The caller holds a game slot (codex\game-slot.ps1). Every process from this run's folder is stopped at the end.
$ErrorActionPreference='Stop'
$workspace='F:\KK-native-renderer\wt\bench-gpu'
$stamp=Get-Date -Format yyyyMMdd-HHmmss
$run="F:\KK-native-renderer\runs\task172-$Api-$Mode$(if($Label){"-$Label"})-$stamp"
New-Item -ItemType Directory -Path $run -Force | Out-Null
$pkg='F:\KK-native-renderer\testbuilds\beta6-test6-1016-1201\KingKong'
$src="$workspace\kk\out\build\$Build"
Get-ChildItem -LiteralPath $pkg -File | Where-Object {$_.Name -match '\.(dll|list)$'} | Copy-Item -Destination $run
Copy-Item -LiteralPath "$pkg\glyphs" -Destination "$run\glyphs" -Recurse
Copy-Item -LiteralPath "$src\king_kong.exe" -Destination $run -Force
Get-ChildItem -LiteralPath $src -Filter '*.dll' | Where-Object {$_.Name -notmatch '^rexgpu-(xenos|classic)'} | Copy-Item -Destination $run -Force
Copy-Item -LiteralPath 'F:\KK-native-renderer\testbuilds\bench-routes\route-45s-1015-1609.kkbr' -Destination "$run\benchmark-route.kkbr"
robocopy 'F:\KK-native-renderer\userdata' "$run\userdata" /E /NFL /NDL /NJH /NJS /NP | Out-Null
robocopy 'F:\KK-native-renderer\runs\task147-cache-beta6' "$run\cache" /E /NFL /NDL /NJH /NJS /NP | Out-Null
if($LASTEXITCODE -gt 7){throw 'Private copy failed'}
@'
kk_settings_version = 3
kk_render_quality = "custom"
gpu_plugin = "native"
kk_camera_sensitivity = 150
kk_deadzone = 5
kk_vibration_strength = 100
kk_last_version = "2.0.0-beta.5"
'@ | Set-Content -LiteralPath "$run\king_kong.toml" -Encoding ASCII
Get-ChildItem Env: | Where-Object {$_.Name -like 'KK_*' -or $_.Name -like 'REX_DEV_*'} |
 ForEach-Object {[Environment]::SetEnvironmentVariable($_.Name,$null,'Process')}
$env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
$env:KK_DEV_BENCH_GPU_DUMP="$run\gpu-dump.csv"
$arguments=@('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
 "--user_data_root=$run\userdata","--game_data_root=$workspace\kk\assets","--cache_root=$run\cache",
 "--log_file=$run\run.log","--config=$run\king_kong.toml",'--kk_renderer=native','--gpu_plugin=native',
 "--native_backend=$Api",'--kk_check_updates=false','--kk_online_count=false','--kk_discord_status=false')
if($Mode -eq 'launcher') { $env:KK_DEV_LAUNCHER_INPUT='benchmark:1' }
else { $arguments+=@('--kk_launcher=false','--kk_benchmark=true','--kk_benchmark_passes=1') }
$arguments | Set-Content -LiteralPath "$run\arguments.txt"
function Own { @(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'" | Where-Object {$_.ExecutablePath -and $_.ExecutablePath.StartsWith($run,'OrdinalIgnoreCase')}) }
function Others { @(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'" | Where-Object {!$_.ExecutablePath -or !$_.ExecutablePath.StartsWith($run,'OrdinalIgnoreCase')}) }
# The other game slot may be in use (two agent games are allowed); its GPU use is "other programs" here.
if((Others).Count){"Another game is running: $((Others | ForEach-Object {$_.ProcessId}) -join ',')"}
$game=Start-Process -FilePath "$run\king_kong.exe" -ArgumentList $arguments -WorkingDirectory $run -WindowStyle Hidden -PassThru
"LAUNCHED $($game.Id) $run"
$seen=@{}; $deadline=(Get-Date).AddMinutes(9); $doneAt=$null; $result='timeout'
try {
 while((Get-Date) -lt $deadline) {
  Start-Sleep -Seconds 3
  foreach($o in Others){ if(!$seen.ContainsKey(-[int]$o.ProcessId)){ $seen[-[int]$o.ProcessId]=1; "$(Get-Date -Format HH:mm:ss) OTHER GAME pid=$($o.ProcessId) $($o.ExecutablePath)" | Add-Content "$run\processes.txt" } }
  foreach($p in Own){ if(!$seen.ContainsKey([int]$p.ProcessId)){ $seen[[int]$p.ProcessId]=1
    "$(Get-Date -Format HH:mm:ss) pid=$($p.ProcessId) parent=$($p.ParentProcessId) $($p.CommandLine.Substring([Math]::Max(0,$p.CommandLine.Length-160)))" | Add-Content "$run\processes.txt" } }
  $logs=Get-ChildItem -LiteralPath $run -Filter 'run*.log' -ErrorAction SilentlyContinue
  $text=($logs | ForEach-Object { $s=[IO.File]::Open($_.FullName,'Open','Read','ReadWrite'); $r=[IO.StreamReader]::new($s); try{$r.ReadToEnd()}finally{$r.Dispose()} }) -join "`n"
  if(!$doneAt -and ($text.Contains('KK benchmark: complete') -or $text.Contains('KK benchmark: cancelled'))){$doneAt=Get-Date}
  if($doneAt -and ((Get-Date)-$doneAt).TotalSeconds -gt 12){$result='complete'; break}
  if(!(Own).Count){$result='all exited'; break}
 }
} finally {
 foreach($p in Own){ Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
 Start-Sleep -Seconds 2
 "RESULT $result; own left: $((Own).Count)"
}
