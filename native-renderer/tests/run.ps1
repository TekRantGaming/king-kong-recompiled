<#
.SYNOPSIS
  Golden-frame test runner for the King Kong renderer: plays the scripted scenes, captures the game's own
  frames and frame logs, and compares them with the golden set (report.html).

.EXAMPLE
  tests\run.ps1 -Plugin native -Scenes all          # capture with rexgpu-native, compare, write the report
  tests\run.ps1 -Plugin xenos -Golden -Force        # (re)capture the golden set with today's renderer
  tests\run.ps1 -Plugin xenos -Scenes title,pause   # a few scenes only (scene or run names)
  tests\run.ps1 -Plugin native -Tour                # the chapter tour (tests/tour.json): all 42 chapters
  tests\run.ps1 -Tour -RecordPipelines              # the tour on the native plugin, recording every pipeline

  Exit codes: 0 pass, 1 fail, 2 missing frames, 3 the game was busy, 4 setup error. See docs/testing.md.
#>
[CmdletBinding()]
param(
  [string]$Plugin = 'xenos',
  [string[]]$Scenes = @('all'),
  [switch]$Golden,
  [switch]$Force,
  [string]$Out = '',
  [string]$GoldenRoot = 'F:\KK-native-renderer\golden',
  [string]$RunsRoot = 'F:\KK-native-renderer\runs',
  [string]$Exe = '',
  [string]$GameData = '',
  [string]$UserData = 'F:\KK-native-renderer\userdata',
  [string]$CacheRoot = 'F:\KK-native-renderer\cache',
  [string]$LockFile = 'F:\KK-native-renderer\game.lock',
  [string]$LockName = 'tests/run.ps1',
  [int]$WaitMinutes = 0,
  # Seconds between two checks while waiting for the game (another runner may be using it).
  [int]$WaitPollSeconds = 300,
  # Minutes to wait after each launch before the next one, so others sharing the game get a turn.
  [double]$PauseMinutes = 0,
  [string]$Label = '',
  [string]$Python = 'python',
  [switch]$NoCompare,
  [switch]$NoFrameLog,
  # Extra game arguments for every launch (for example --native_dump_frame=200).
  [string[]]$ExtraArgs = @(),
  # Another scene file than tests/scenes.json (probes for new scenes).
  [string]$ScenesFile = '',
  # The chapter tour: tests/tour.json (every chapter-select entry, several chapters per launch).
  [switch]$Tour,
  # Native plugin with its pipeline cache recording into a fresh file per launch; the files are collected in
  # <run folder>\pipelines\<run>.pipelines (a prewarm list for every chapter played).
  [switch]$RecordPipelines
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$compare = Join-Path $PSScriptRoot 'compare.py'
# -Scenes title,pause arrives as an array from PowerShell and as one "title,pause" string from cmd or bash.
$SceneList = ($Scenes -join ',')
if ($Tour -and -not $ScenesFile) { $ScenesFile = Join-Path $PSScriptRoot 'tour.json' }
if ($RecordPipelines) {
  if ($PSBoundParameters.ContainsKey('Plugin') -and $Plugin -ne 'native') {
    Write-Output '-RecordPipelines needs the native plugin'; exit 4
  }
  $Plugin = 'native'
  if ($Golden) { Write-Output '-RecordPipelines and -Golden do not go together'; exit 4 }
}
if (-not $Exe) {
  # kk-dev has the Xenos plugin only; the native plugin comes with the kk-dev-vulkan build.
  $Exe = Join-Path $repo 'kk\out\build\kk-dev\king_kong.exe'
  $vk = Join-Path $repo 'kk\out\build\kk-dev-vulkan\king_kong.exe'
  if (-not (Get-ChildItem -Path (Split-Path $Exe) -Filter "rexgpu-$Plugin*.dll" -ErrorAction SilentlyContinue) -and
      (Get-ChildItem -Path (Split-Path $vk) -Filter "rexgpu-$Plugin*.dll" -ErrorAction SilentlyContinue)) { $Exe = $vk }
}
if (-not $GameData) { $GameData = Join-Path $repo 'kk\assets' }
foreach ($p in @($Exe, $GameData, $UserData)) {
  if (-not (Test-Path $p)) { Write-Output "not found: $p"; exit 4 }
}
# The runtime loads rexgpu-<name>.dll (rexgpu-<name>rd.dll in RelWithDebInfo builds) from the game's folder.
$exeDir = Split-Path $Exe
if (-not (Get-ChildItem -Path $exeDir -Filter "rexgpu-$Plugin*.dll" -ErrorAction SilentlyContinue)) {
  Write-Output "no rexgpu-$Plugin.dll beside $Exe"
  exit 4
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ($Golden) { $root = $GoldenRoot } elseif ($Out) { $root = $Out } else { $root = Join-Path $RunsRoot "$Plugin-$stamp" }
New-Item -ItemType Directory -Force $root | Out-Null
if (-not $Label) { $Label = "$Plugin $stamp" }

function Invoke-Py([string[]]$PyArgs) {
  if ($ScenesFile) { $PyArgs = @('--scenes-file', $ScenesFile) + $PyArgs }
  $out = & $Python -I $compare @PyArgs
  return @{ code = $LASTEXITCODE; out = ($out -join "`n") }
}

# What to launch: one game run per group of scenes (see scenes.json).
$planArgs = @('plan', '--scenes', $SceneList, '--plugin', $Plugin, '--golden-root', $GoldenRoot)
if ($Golden) { $planArgs += '--golden' }
if ($NoFrameLog) { $planArgs += '--no-frame-log' }
$r = Invoke-Py $planArgs
if ($r.code -ne 0) { Write-Output $r.out; exit 4 }
$plan = $r.out | ConvertFrom-Json

if ($Golden -and -not $Force) {
  foreach ($run in $plan.runs) {
    foreach ($s in $run.scenes) {
      if (Test-Path (Join-Path $GoldenRoot "$s\meta.json")) {
        Write-Output "golden set for '$s' exists in $GoldenRoot; add -Force to capture it again"
        exit 4
      }
    }
  }
}

function Wait-GameFree {
  # At most one copy of the game may run on this PC: never start a second one, never stop someone else's.
  # A lock file saying "hold" or "user" (someone is about to use the game, or playing it) counts as busy too.
  $deadline = (Get-Date).AddMinutes($WaitMinutes)
  while ($true) {
    $busy = Get-Process king_kong -ErrorAction SilentlyContinue
    $held = $false
    if (Test-Path $LockFile) {
      $lockText = [string](Get-Content -Path $LockFile -TotalCount 1 -ErrorAction SilentlyContinue)
      $held = $lockText -match '^\s*(hold|user)'
    }
    if (-not $busy -and -not $held) { return $true }
    if ((Get-Date) -ge $deadline) { return $false }
    if ($busy) { $why = "the game is running (pid $($busy.Id -join ','))" } else { $why = "the lock file says '$lockText'" }
    Write-Output ("{0:HH:mm} {1}; checking again in {2} s" -f (Get-Date), $why, $WaitPollSeconds)
    Start-Sleep -Seconds $WaitPollSeconds
  }
}

function Set-Lock([string]$Text) {
  if (Test-Path (Split-Path $LockFile)) { Set-Content -Path $LockFile -Value $Text -Encoding ascii }
}

function Format-Arg([string]$a) { if ($a -match '\s') { '"' + $a + '"' } else { $a } }

$status = 0
$launches = 0
foreach ($run in $plan.runs) {
  if ($launches -gt 0 -and $PauseMinutes -gt 0) {
    # Leave the game to others between launches (the PC's game is shared).
    Write-Output ("{0:HH:mm} pausing {1} minutes before the next launch" -f (Get-Date), $PauseMinutes)
    Start-Sleep -Seconds ([int]($PauseMinutes * 60))
  }
  $launches++
  $runDir = Join-Path $root ("_runs\{0}-{1}" -f $run.name, (Get-Date -Format 'yyyyMMdd-HHmmss'))
  New-Item -ItemType Directory -Force $runDir | Out-Null
  if (-not (Wait-GameFree)) {
    Write-Output "the game is already running; nothing launched for run '$($run.name)'"
    exit 3
  }
  # A private copy of the test save, so runs never change it and always start from the same state.
  $saveCopy = Join-Path $runDir 'userdata'
  & robocopy $UserData $saveCopy /E /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) { Write-Output "robocopy of the test save failed ($LASTEXITCODE)"; exit 4 }

  # Environment: clear every KK_ / REX_DEV_ variable inherited from this shell, then set the run's own.
  $saved = @{}
  Get-ChildItem Env: | Where-Object { $_.Name -like 'KK_*' -or $_.Name -like 'REX_DEV_*' } | ForEach-Object {
    $saved[$_.Name] = $_.Value
    [Environment]::SetEnvironmentVariable($_.Name, $null, 'Process')
  }
  foreach ($kv in $run.env.PSObject.Properties) {
    [Environment]::SetEnvironmentVariable($kv.Name, [string]$kv.Value, 'Process')
    if (-not $saved.ContainsKey($kv.Name)) { $saved[$kv.Name] = $null }
  }
  # SDL3 hints: show the game window without taking the focus (someone may be using the PC meanwhile). The
  # captures are the game's own frames, so a window behind others is captured the same.
  foreach ($name in @('SDL_WINDOW_ACTIVATE_WHEN_SHOWN', 'SDL_WINDOW_ACTIVATE_WHEN_RAISED')) {
    if (-not $saved.ContainsKey($name)) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
    [Environment]::SetEnvironmentVariable($name, '0', 'Process')
  }
  # -ExtraArgs replace the scene file's arguments of the same name (the game keeps the first of two).
  $launchExtra = @($ExtraArgs)
  $pipeDir = Join-Path $runDir 'pipelines'
  if ($RecordPipelines) {
    # A fresh, empty cache file per launch: nothing is prewarmed, so the timeline stays that of a cold run.
    New-Item -ItemType Directory -Force $pipeDir | Out-Null
    $launchExtra += @('--native_pipeline_cache=true', "--native_pipeline_cache_dir=$pipeDir")
  }
  $extraNames = @($launchExtra | ForEach-Object { ($_ -split '=', 2)[0] })
  $runArgs = @($run.args | Where-Object { $extraNames -notcontains (($_ -split '=', 2)[0]) })
  $gameArgs = @("--game_data_root=$GameData", "--user_data_root=$saveCopy", "--cache_root=$CacheRoot",
    "--log_file=$(Join-Path $runDir 'game.log')") + $runArgs + $launchExtra | ForEach-Object { Format-Arg $_ }
  $log = Join-Path $runDir 'game.log'
  $proc = $null
  $started = Get-Date
  try {
    Set-Lock ("{0} ({1}, run {2}) {3}" -f $LockName, $Plugin, $run.name, (Get-Date -Format s))
    Write-Output ("{0:HH:mm:ss} run {1}: scenes {2}, {3} shots, up to {4} s" -f (Get-Date), $run.name,
      ($run.scenes -join ','), $run.expected.Count, $run.limit_s)
    $proc = Start-Process -FilePath $Exe -ArgumentList $gameArgs -WorkingDirectory $runDir -PassThru
    while ($true) {
      Start-Sleep -Seconds 1
      if ($proc.HasExited) { Write-Output "  the game exited by itself (code $($proc.ExitCode))"; break }
      $done = $true
      foreach ($f in $run.expected) { if (-not (Test-Path (Join-Path $runDir $f))) { $done = $false; break } }
      if ($done -and $run.frame_log_frames -gt 0) {
        $ends = @(Select-String -Path $log -Pattern 'Frame log: frame end' -SimpleMatch -ErrorAction SilentlyContinue)
        if ($ends.Count -lt $run.frame_log_frames) { $done = $false }
      }
      if ($done) { Start-Sleep -Seconds 1; break }
      if (((Get-Date) - $started).TotalSeconds -gt $run.limit_s) { Write-Output '  time limit reached'; break }
    }
  } finally {
    if ($proc -and -not $proc.HasExited) {
      # Ask the window to close first (logs and the shader cache are written on exit), then stop it.
      [void]$proc.CloseMainWindow()
      if (-not $proc.WaitForExit(15000)) { Stop-Process -InputObject $proc -Force }
    }
    Set-Lock 'free'
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], 'Process') }
  }
  # --name=value forms: Windows PowerShell drops empty arguments when it calls a program.
  $collect = @('collect', '--run', $run.name, '--run-dir', $runDir, '--out-root', $root, '--scenes', $SceneList,
    "--plugin=$Plugin", "--label=$Label", "--trigger=$($run.env.REX_DEV_FRAME_LOG)")
  if ($Golden) { $collect += '--golden' }
  $r = Invoke-Py $collect
  Write-Output ("  {0:N0} s; {1}" -f ((Get-Date) - $started).TotalSeconds, $r.out)
  if ($r.code -ne 0) { $status = 2 }
  if ($RecordPipelines) {
    # The plugin names the file game.pipelines when the folder comes from --native_pipeline_cache_dir.
    $file = Get-ChildItem -Path $pipeDir -Filter '*.pipelines' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($file) {
      $collected = Join-Path $root 'pipelines'
      New-Item -ItemType Directory -Force $collected | Out-Null
      $dest = Join-Path $collected "$($run.name).pipelines"
      Copy-Item -LiteralPath $file.FullName -Destination $dest -Force
      $bytes = [IO.File]::ReadAllBytes($file.FullName)
      $records = 0
      if ($bytes.Length -ge 16) {
        $size = [BitConverter]::ToUInt32($bytes, 8)  # header: magic, version, record size, byte order
        if ($size -gt 0) { $records = [math]::Floor(($bytes.Length - 16) / $size) }
      }
      Write-Output ("  pipelines: {0} records -> {1}" -f $records, $dest)
    } else {
      Write-Output "  pipelines: no file in $pipeDir"
      $status = 2
    }
  }
}
if ($RecordPipelines) { Write-Output "pipeline descriptions of every launch: $(Join-Path $root 'pipelines')" }

if ($Golden) {
  Write-Output "golden set written to $root"
  exit $status
}
if ($NoCompare) { exit $status }
$r = Invoke-Py @('compare', '--golden-root', $GoldenRoot, '--test-root', $root, '--scenes', $SceneList, '--plugin', $Plugin)
Write-Output $r.out
if ($r.code -ne 0) { exit $r.code }
exit $status
