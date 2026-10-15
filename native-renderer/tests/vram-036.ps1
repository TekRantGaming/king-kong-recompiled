param([int[]]$Budgets=@(4,6,8), [string[]]$Cases=@('ch0','ch8','ch27','tour'),
      [string[]]$Controls=@('before','after'), [string]$Scale='2560x1440',
      [switch]$Pressure, [switch]$Gtao,
      [ValidateSet('vulkan','d3d12')][string]$Backend='vulkan',
      [string]$PackRoot='F:\KK-native-renderer\runs\task048-after-d3d12-dlss-0-fresh-20261014-072055\cache\native-shaders',
      [ValidatePattern('^task[0-9]+$')][string]$Task='task036',
      [ValidateSet('','off','low','medium','high')][string]$Gi='', [string]$Shots='60,120')
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path "$PSScriptRoot/../..").Path
$lockPath='F:\KK-native-renderer\runs\codex-game.lock'
$build="$workspace\kk\out\build\kk-dev-vulkan"
function Acquire-Game {
  while ($true) {
    $pollSeconds=30
    # Atomically check and claim the common lock while holding its file handle.
    $file=$null
    try {
      $file=[IO.File]::Open($lockPath,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
      $bytes=New-Object byte[] ([int]$file.Length)
      $null=$file.Read($bytes,0,$bytes.Length)
      $state=[Text.Encoding]::UTF8.GetString($bytes).Trim([char]0xFEFF).Trim()
      if ($state.StartsWith('user')) { throw 'USER_PAUSE: no game launches permitted' }
      $priorityPath='F:\KK-native-renderer\runs\codex-game.priority'
      $priority=if (Test-Path $priorityPath) { (Get-Content $priorityPath -Raw).Trim() } else { $null }
      $games=@(Get-Process king_kong -ErrorAction SilentlyContinue)
      foreach ($existingGame in $games) {
        $existingPath=$existingGame.Path
        if (!$existingPath -or ($existingPath -notlike 'F:\KK-native-renderer\runs\*' -and
                              $existingPath -notlike 'F:\KK-native-renderer\wt\*')) {
          $pollSeconds=60
        }
      }
      if (($state -eq 'free' -or !$state) -and $games.Count -eq 0 -and (!$priority -or $priority -eq $Task)) {
        if (!(Get-Process kkbg2 -ErrorAction SilentlyContinue)) { throw 'kkbg2 is required' }
        $claim=[Text.Encoding]::UTF8.GetBytes("$Task $(Get-Date -Format o)")
        $file.SetLength(0); $file.Write($claim,0,$claim.Length); $file.Flush()
        return
      }
    } catch [IO.IOException] {
      # Another runner owns the handle or the game lock. Poll per GAME-RULES.
    } finally { if ($file) { $file.Dispose() } }
    Write-Output "WAIT $(Get-Date -Format o) game lock busy"
    Start-Sleep -Seconds $pollSeconds
  }
}
foreach ($budget in $Budgets) {
  if ($budget -notin 4,6,8) { throw 'Budget must be 4, 6 or 8 GiB' }
  foreach ($case in $Cases) { foreach ($control in $Controls) {
    if ($control -notin 'before','after') { throw 'Control must be before or after' }
    $tag=if ($Pressure) { '-pressure' } else { '' }
    if ($Gtao) { $tag+='-gtao' }
    if ($Backend -ne 'vulkan') { $tag+="-$Backend" }
    $run="F:\KK-native-renderer\runs\$Task-$control-$budget-$case$tag-$(Get-Date -Format yyyyMMdd-HHmmss)"
    New-Item -ItemType Directory -Path $run | Out-Null
    Get-ChildItem $build -File | Where-Object Extension -In '.exe','.dll' | Copy-Item -Destination $run
    Copy-Item "$build\glyphs" $run -Recurse
    foreach ($pack in 'kkshaders-spirv.pack','kkshaders-dxil.pack') {
      if (Test-Path "$PackRoot\$pack") { Copy-Item "$PackRoot\$pack" $run }
    }
    $requiredPack=if ($Backend -eq 'vulkan') { 'kkshaders-spirv.pack' } else { 'kkshaders-dxil.pack' }
    if (!(Test-Path "$run\$requiredPack")) { throw "Missing $requiredPack" }
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
      '--kk_launcher=false','--gpu_plugin=native',"--native_backend=$Backend","--native_render_scale=$Scale",
      '--native_upscaler=off','--kk_frame_rate=30','--native_debug=32768','--native_stutter_stats=true',
      '--native_validation=true','--kk_check_updates=false','--kk_online_count=false','--kk_discord_status=false')
    $args+='--log_max_file_size_mb=500'
    if ($Gi) { $args+="--native_gi=$Gi" }
    if ($Gtao) { $args+='--ao_mode=1','--native_ao_method=gtao','--native_ao_quality=full' }
    else { $args+='--ao_mode=0' }
    if ($case -eq 'tour') {
      # Same-process 8 -> 9 slice, using the existing developer end-of-chapter
      # aid. The old menu tour scripts no longer match the port's new menus.
      $env:KK_DEV_SCRIPT='0:SKIP/120'
      $env:KK_DEV_SHOTS_FROM='menu'; $env:KK_DEV_SHOTS='30,120'
      $env:KK_DEV_START_END='40'; $env:KK_DEV_VRAM_TOUR='1'; $env:KK_DEV_JUMP_TRACE='1'
      $args+='--kk_start=chapter:0:8'
      $limit=240
    } elseif ($case -match '^ch(0|8|27)$') {
      $chapter=[int]$Matches[1]
      $args+="--kk_start=chapter:0:$chapter"
      $env:KK_DEV_SCRIPT='0:SKIP/120'; $env:KK_DEV_SHOTS=$Shots; $env:KK_DEV_SHOTS_FROM='menu'
      # Capture time is the loaded-level clock. Allow a bounded cold pipeline
      # startup as well as Venture's intro without shortening the 120s sample.
      $limit=240
    } else { throw 'Unknown case' }
    @{budget_gib=$budget;case=$case;control=$control;scale=$Scale;pressure=[bool]$Pressure;gtao=[bool]$Gtao;
      pressure_delay_s=$(if ($Pressure) {20} else {0});args=$args;
      tour_kind=$(if ($case -eq 'tour') {'story8-9'} else {''});
      backend=$Backend;
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
      if ((Get-Content $lockPath -Raw).Trim().StartsWith("$Task ")) { Set-Content $lockPath 'free' }
    }
    Write-Output "DONE $run exit=$($game.ExitCode) seconds=$($clock.Elapsed.TotalSeconds)"
    if ($game.ExitCode -ne 0) { throw "Game failed with exit $($game.ExitCode): $run" }
    if (!(Test-Path "$run\shot_1200.bmp")) { throw "Two-minute capture missing: $run" }
  } }
}
