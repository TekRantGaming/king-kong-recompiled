param([int[]]$Budgets=@(4,6,8), [string[]]$Cases=@('ch0','ch8','ch27','tour'),
      [string[]]$Controls=@('before','after'), [string]$Scale='2560x1440',
      [switch]$Pressure, [switch]$Gtao,
      [string]$PackRoot='F:\KK-native-renderer\runs\task048-after-d3d12-dlss-0-fresh-20261014-072055\cache\native-shaders')
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path "$PSScriptRoot/../..").Path
$lockPath='F:\KK-native-renderer\runs\codex-game.lock'
$build="$workspace\kk\out\build\kk-dev-vulkan"
function Acquire-Game {
  while ($true) {
    # Atomically check and claim the common lock while holding its file handle.
    $file=$null
    try {
      $file=[IO.File]::Open($lockPath,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
      $bytes=New-Object byte[] ([int]$file.Length)
      $null=$file.Read($bytes,0,$bytes.Length)
      $state=[Text.Encoding]::UTF8.GetString($bytes).Trim([char]0xFEFF).Trim()
      if ($state.StartsWith('user')) { throw 'USER_PAUSE: no game launches permitted' }
      $games=@(Get-Process king_kong -ErrorAction SilentlyContinue)
      if (($state -eq 'free' -or !$state) -and $games.Count -eq 0) {
        if (!(Get-Process kkbg2 -ErrorAction SilentlyContinue)) { throw 'kkbg2 is required' }
        $claim=[Text.Encoding]::UTF8.GetBytes("task036 $(Get-Date -Format o)")
        $file.SetLength(0); $file.Write($claim,0,$claim.Length); $file.Flush()
        return
      }
    } catch [IO.IOException] {
      # Another runner owns the handle or the game lock. Poll per GAME-RULES.
    } finally { if ($file) { $file.Dispose() } }
    Write-Output "WAIT $(Get-Date -Format o) game lock busy"
    Start-Sleep -Seconds 30
  }
}
foreach ($budget in $Budgets) {
  if ($budget -notin 4,6,8) { throw 'Budget must be 4, 6 or 8 GiB' }
  foreach ($case in $Cases) { foreach ($control in $Controls) {
    if ($control -notin 'before','after') { throw 'Control must be before or after' }
    $tag=if ($Pressure) { '-pressure' } else { '' }
    if ($Gtao) { $tag+='-gtao' }
    $run="F:\KK-native-renderer\runs\task036-$control-$budget-$case$tag-$(Get-Date -Format yyyyMMdd-HHmmss)"
    New-Item -ItemType Directory -Path $run | Out-Null
    Get-ChildItem $build -File | Where-Object Extension -In '.exe','.dll' | Copy-Item -Destination $run
    Copy-Item "$build\glyphs" $run -Recurse
    foreach ($pack in 'kkshaders-spirv.pack','kkshaders-dxil.pack') { Copy-Item "$PackRoot\$pack" $run }
    Copy-Item 'F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc\bin\x64\dxcompiler.dll' $run
    robocopy F:\KK-native-renderer\userdata "$run\userdata" /E /NFL /NDL /NJH /NJS | Out-Null
    if ($LASTEXITCODE -ge 8) { throw 'User data copy failed' }
    Get-ChildItem Env: | Where-Object Name -Like 'KK_DEV_*' | ForEach-Object { Remove-Item "Env:$($_.Name)" }
    $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0'; $env:SDL_WINDOW_ACTIVATE_WHEN_RAISED='0'
    $env:KK_DEV_VRAM_BUDGET_GB="$budget"
    $env:KK_DEV_VRAM_CONTROL=if ($control -eq 'before') {'0'} else {'1'}
    if ($Pressure) { $env:KK_DEV_VRAM_PRESSURE='1' }
    $env:KK_DEV_DT_LOG='0,240'; $env:KK_DEV_DLSS_LOG="$run\render_frames.csv"
    $env:KK_DEV_SHOTS_DIR=$run
    $env:KK_DEV_AUTOSKIP='1'
    $args=@('--fullscreen=false','--window_width=1280','--window_height=720','--kk_fullscreen_mode=desktop',
      "--user_data_root=$run\userdata","--cache_root=$run\cache",
      "--game_data_root=$($workspace.Replace('\','/'))/kk/assets","--log_file=$run\run.log",
      '--kk_launcher=false','--gpu_plugin=native','--native_backend=vulkan',"--native_render_scale=$Scale",
      '--native_upscaler=off','--kk_frame_rate=30','--native_debug=32768','--native_stutter_stats=true',
      '--native_validation=true','--kk_check_updates=false','--kk_online_count=false','--kk_discord_status=false')
    $args+='--log_max_file_size_mb=500'
    if ($Gtao) { $args+='--ao_mode=1','--native_ao_method=gtao','--native_ao_quality=full' }
    else { $args+='--ao_mode=0' }
    if ($case -eq 'tour') {
      # Two-minute slice of the existing long tour B, same menu clock and input.
      $tour=Get-Content "$PSScriptRoot\tour.json" -Raw | ConvertFrom-Json
      $env:KK_DEV_SCRIPT=($tour.runs.tourB.script.Split(',') | Where-Object { [double]($_.Split(':')[0]) -le 120 }) -join ','
      $env:KK_DEV_SHOTS_FROM='menu'; $env:KK_DEV_SHOTS='60,119'
      $env:KK_DEV_FREEZE_SKIP='10,29'
      $args+='--kk_cheats=true','--kk_cheat_chapters=true','--kk_start='
      $limit=135
    } elseif ($case -match '^ch(0|8|27)$') {
      $chapter=[int]$Matches[1]
      $args+="--kk_start=chapter:0:$chapter"
      $env:KK_DEV_SCRIPT='0:SKIP/120'; $env:KK_DEV_SHOTS='60,120'; $env:KK_DEV_SHOTS_FROM='menu'
      $limit=if ($chapter -eq 0) {190} else {135}
    } else { throw 'Unknown case' }
    @{budget_gib=$budget;case=$case;control=$control;scale=$Scale;pressure=[bool]$Pressure;gtao=[bool]$Gtao;args=$args;
      script=$env:KK_DEV_SCRIPT;limit=$limit;dll_sha256=(Get-FileHash "$run\rexgpu-native.dll").Hash;
      pack_root=$PackRoot} | ConvertTo-Json -Depth 4 | Set-Content "$run\test.json"
    Acquire-Game
    $game=$null
    try {
      $game=Start-Process "$run\king_kong.exe" -ArgumentList $args -WorkingDirectory $run -WindowStyle Hidden -PassThru
      Write-Output "RUN $run PID $($game.Id)"
      $clock=[Diagnostics.Stopwatch]::StartNew()
      while (!$game.HasExited -and $clock.Elapsed.TotalSeconds -lt $limit) {
        if (Test-Path "$run\shot_1200.bmp") { break }
        Start-Sleep -Seconds 2
      }
    } finally {
      if ($game -and !$game.HasExited) {
        $null=$game.CloseMainWindow()
        if (!$game.WaitForExit(10000)) { Stop-Process -Id $game.Id; $game.WaitForExit() }
      }
      # Do not overwrite a user pause request written during this run.
      if ((Get-Content $lockPath -Raw).Trim().StartsWith('task036 ')) { Set-Content $lockPath 'free' }
    }
    Write-Output "DONE $run exit=$($game.ExitCode) seconds=$($clock.Elapsed.TotalSeconds)"
  } }
}
