param([int[]]$Entries = @(3), [int[]]$AoModes = @(1), [string]$Label = 'before', [int]$Seconds = 100, [string[]]$ExtraArgs = @())
$ErrorActionPreference = 'Stop'
$root = 'F:\KK-native-renderer\wt\codex-white'
$build = "$root\kk\out\build\kk-dev-vulkan"
$lockPath = 'F:\KK-native-renderer\runs\codex-game.lock'
foreach ($entry in $Entries) { foreach ($aoMode in $AoModes) {
  $run = 'F:\KK-native-renderer\runs\task015-' + $Label + '-e' + $entry + '-ao' + $aoMode + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
  New-Item -ItemType Directory -Path $run | Out-Null
  Get-ChildItem -LiteralPath $build -File | Where-Object {$_.Extension -in '.exe','.dll'} | Copy-Item -Destination $run
  Copy-Item -LiteralPath "$root\kk\glyphs" -Destination "$run\glyphs" -Recurse
  Copy-Item -LiteralPath 'F:\KK-native-renderer\wt\04-backend\kk\out\build\kk-dev-vulkan\kkshaders-spirv.pack' -Destination $run
  Copy-Item -LiteralPath 'F:\KK-native-renderer\wt\04-backend\kk\out\build\kk-dev-vulkan\dxcompiler.dll' -Destination $run
  robocopy 'F:\KK-native-renderer\userdata' "$run\userdata" /E /NFL /NDL /NJH /NJS | Out-Null
  if ($LASTEXITCODE -ge 8) {throw 'userdata copy failed'}
  while ($true) {
    if (!(Test-Path -LiteralPath $lockPath)) { Set-Content -LiteralPath $lockPath -Value 'free' }
    $locked = $false
    $stream = $null
    try {
      $stream = [IO.File]::Open($lockPath, 'Open', 'ReadWrite', 'None')
      $reader = New-Object IO.StreamReader($stream, [Text.Encoding]::UTF8, $true, 1024, $true)
      $state = $reader.ReadToEnd().Trim(); $reader.Dispose()
      $ours = @(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'" | Where-Object {$_.ExecutablePath -like 'F:\KK-native-renderer\runs\*' -or $_.ExecutablePath -like 'F:\KK-native-renderer\wt\*'})
      if ($state -eq 'free' -and $ours.Count -eq 0) {
        $stream.SetLength(0); $stream.Position = 0
        $writer = New-Object IO.StreamWriter($stream, [Text.Encoding]::UTF8, 1024, $true)
        $writer.Write('task015 ' + (Get-Date -Format o)); $writer.Flush(); $writer.Dispose()
        $locked = $true
      }
    } catch [IO.IOException] {} finally {if ($stream) {$stream.Dispose()}}
    if ($locked) {break}
    Write-Output ('waiting for codex-game.lock ' + (Get-Date -Format T)); Start-Sleep -Seconds 30
  }
  $proc = $null
  try {
    Get-ChildItem Env: | Where-Object {$_.Name -like 'KK_*' -or $_.Name -like 'REX_DEV_*'} | ForEach-Object {Remove-Item -LiteralPath ('Env:' + $_.Name)}
    $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
    $env:KK_DEV_AUTOSKIP='1'; $env:KK_DEV_SHOTS_FROM='menu'; $env:KK_DEV_SHOTS_DIR=$run
    $env:KK_DEV_SHOTS = ((50..350 | ForEach-Object {($_ / 5.0).ToString('0.0',[Globalization.CultureInfo]::InvariantCulture)}) -join ',')
    $env:KK_DEV_SCRIPT='1:SKIP,3:SKIP,5:SKIP,7:SKIP,9:SKIP,11:SKIP,13:SKIP,15:SKIP,17:SKIP,19:SKIP,21:SKIP,23:SKIP,25:SKIP,27:SKIP,29:SKIP,31:SKIP,33:SKIP,35:SKIP'
    $env:KK_DEV_WANDER='2'
    $args = @('--kk_launcher=false', "--kk_start=chapter:0:$entry", '--gpu_plugin=native', '--fullscreen=false', '--kk_fullscreen_mode=desktop', '--window_width=1280', '--window_height=720', '--kk_frame_rate=30', '--kk_cheats=true', '--kk_cheat_chapters=true', '--kk_cheat_ammo=true', '--native_render_scale=1', '--native_upscaler=off', '--native_ao_method=gtao', '--native_ao_apply=ambient', "--ao_mode=$aoMode", '--ao_strength=3', "--user_data_root=$run\userdata", "--cache_root=$run\cache", "--game_data_root=F:/KK-native-renderer/wt/codex-white/kk/assets", "--log_file=$run\run.log", '--log_max_file_size_mb=500') + $ExtraArgs
    $overrideNames = @($ExtraArgs | ForEach-Object {($_ -split '=',2)[0]})
    if ($overrideNames.Count) {
      $baseArgs = $args[0..($args.Count - $ExtraArgs.Count - 1)]
      $args = @($baseArgs | Where-Object {$overrideNames -notcontains (($_ -split '=',2)[0])}) + $ExtraArgs
    }
    $args | Set-Content -LiteralPath "$run\arguments.txt"
    Write-Output ("launch $run")
    $proc = Start-Process -FilePath "$run\king_kong.exe" -ArgumentList $args -WorkingDirectory $run -WindowStyle Hidden -PassThru
    $proc.Id | Set-Content -LiteralPath "$run\pid.txt"
    $start = Get-Date
    while (!$proc.HasExited -and ((Get-Date)-$start).TotalSeconds -lt $Seconds) { Start-Sleep -Seconds 2 }
  } finally {
    if ($proc -and !$proc.HasExited) { [void]$proc.CloseMainWindow(); if (!$proc.WaitForExit(15000)) {Stop-Process -Id $proc.Id -Force; $proc.WaitForExit()} }
    Set-Content -LiteralPath $lockPath -Value 'free'
  }
  $shots = @(Get-ChildItem -LiteralPath $run -Filter 'shot_*.bmp').Count
  $placeholders = @(Select-String -LiteralPath "$run\run.log" -Pattern 'texture placeholder real draw').Count
  Write-Output ("finished $run shots=$shots placeholders=$placeholders exit=$($proc.ExitCode)")
}}
