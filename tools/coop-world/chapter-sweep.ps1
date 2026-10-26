# Authorized task runner: runs the existing two-window runner once per zero-based chapter entry.
# C76 authorized work mode: -WorldDiff -Checkpoints scores native WAN snapshots.
# Examples: & .\chapter-sweep.ps1 -Entries 1,2,3 -Seconds 150
# powershell -NoProfile -File .\chapter-sweep.ps1 -Entries '1,2,3'
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string[]]$Entries,
    [string[]]$Starts = @(),
    [string]$StoryLayout = '',
    [switch]$Stationary,
    [ValidateRange(15,86400)][int]$Seconds = 150,
    [ValidateRange(70,180)][int]$CheckpointSeconds = 90,
    [string]$Worktree = 'F:\kk-native-coop\kk-recomp',
    [switch]$FacePartner,
    [switch]$WorldDiff,
    [switch]$Checkpoints,
    [ValidateRange(0,32)][int]$CheckpointLimit = 3,
    [ValidateRange(1,256)][int]$MaxRuns = 64,
    [string]$DeadlineUtc = '',
    [string]$ResumeManifest = '',
    [ValidateRange(0,60)][int]$RetryBlockedSeconds = 0
)
$ErrorActionPreference = 'Stop'
$coopRoot = 'F:\kk-native-coop'
$runner = Join-Path $coopRoot 'tools\coop-run.ps1'
$pythonExe = 'C:\Users\TekRa\AppData\Local\Programs\Python\Python312\python.exe'
if ($Checkpoints -and -not $WorldDiff) { throw '-Checkpoints requires -WorldDiff' }
if ($WorldDiff -and ($Seconds -gt 180 -or $Seconds -lt 70)) { throw 'World diff runs need 70..180 seconds.' }
$deadline = if ($DeadlineUtc) { [DateTime]::Parse($DeadlineUtc).ToUniversalTime() } else { [DateTime]::UtcNow.AddHours(3) }
$entryList = @($Entries | ForEach-Object {
    foreach ($token in ($_ -split ',')) {
        $entry = 0
        if (-not [int]::TryParse($token.Trim(), [ref]$entry) -or $entry -lt 0 -or $entry -gt 41) {
            throw "Invalid entry '$token': use zero-based chapter entries 0..41."
        }
        $entry
    }
})
$startList = @($Starts | ForEach-Object { $_ -split ',' })
if ($startList.Count -and $startList.Count -ne $entryList.Count) { throw '-Starts must have one checkpoint per entry.' }
foreach ($start in $startList) {
    $parsed=0
    if (-not [int]::TryParse($start,[ref]$parsed) -or $parsed -lt 0 -or $parsed -gt 10000) { throw 'Invalid checkpoint.' }
}
if ($StoryLayout -and $StoryLayout -ne 'all' -and $StoryLayout -notmatch '^[0-9a-fA-F]{1,16}$') { throw 'Invalid story layout selector.' }
$Worktree = (Resolve-Path -LiteralPath $Worktree).ProviderPath.TrimEnd('\')
if (-not $Worktree.StartsWith("$coopRoot\", [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Worktree must be inside F:\kk-native-coop.'
}
if (-not (Test-Path -LiteralPath $runner -PathType Leaf)) { throw "Missing runner: $runner" }
$shellExe = Join-Path $PSHOME 'powershell.exe'
if (-not (Test-Path -LiteralPath $shellExe)) { $shellExe = Join-Path $PSHOME 'pwsh.exe' }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$sweepDir = Join-Path $coopRoot "runs\chapter-sweep-$stamp"
New-Item -ItemType Directory -Path $sweepDir | Out-Null
$manifestPath = Join-Path $sweepDir 'manifest.json'
$harnessDir=Join-Path $sweepDir 'harness'
New-Item -ItemType Directory -Path $harnessDir | Out-Null
$toolHashes=[ordered]@{}
foreach ($toolName in 'chapter-sweep.ps1','world-diff.py','world-sweep-score.py','coop-run.ps1') {
    $toolPath=if ($toolName -eq 'coop-run.ps1') { $runner } else { Join-Path $PSScriptRoot $toolName }
    Copy-Item -LiteralPath $toolPath -Destination $harnessDir
    $toolHashes[$toolName]=(Get-FileHash -LiteralPath $toolPath -Algorithm SHA256).Hash.ToLowerInvariant()
}
$rules = @(Select-String -LiteralPath (Join-Path $Worktree 'kk\src\coop_flow.h') -Pattern '^\s*\{(\d+),ChapterPolicy::(\w+),' | ForEach-Object {
    [ordered]@{ level=[int]$_.Matches[0].Groups[1].Value; policy=$_.Matches[0].Groups[2].Value }
})
if ($rules.Count -ne 42) { throw 'Native chapter policy list did not reconcile to 42 entries.' }
$sourceCommit = (& git -C $Worktree rev-parse HEAD).Trim()
$sourceBranch = (& git -C $Worktree branch --show-current).Trim()
$sourceDirty = @(& git -C $Worktree status --porcelain=v1 --untracked-files=all)
$exePath = Join-Path $Worktree 'kk\out\build\kk-dev-vulkan\king_kong.exe'
$exeSha = if ($WorldDiff) { (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash.ToLowerInvariant() } else { '' }
if ($WorldDiff) {
    $unexpected = @($sourceDirty | Where-Object { $_ -notmatch '^ M kk/(generated/rexglue\.cmake|king_kong_manifest\.toml)$' })
    if ($unexpected) { throw 'World sweep requires committed source. Generated CMake/manifest dirt is retained in evidence.' }
    $dirtyDiffPath=Join-Path $sweepDir 'source-dirty.diff'
    & git -C $Worktree diff --binary HEAD "--output=$dirtyDiffPath"
    if ($LASTEXITCODE -ne 0) { throw 'Could not retain the source dirty diff.' }
}
$records = @()
$scenariosToRun = @($entryList | ForEach-Object { [pscustomobject]@{entry=$_;start=$null;load_proof='chapter-entry-smoke'} })
if ($startList.Count) {
    for ($i=0;$i -lt $scenariosToRun.Count;$i++) {
        $scenariosToRun[$i].start=[int]$startList[$i]
        $scenariosToRun[$i].load_proof='forced-checkpoint-smoke'
    }
}
if ($ResumeManifest) {
    $resumePath=[IO.Path]::GetFullPath($ResumeManifest)
    if (-not $resumePath.StartsWith("$coopRoot\runs\",[StringComparison]::OrdinalIgnoreCase)) { throw 'Resume manifest must be a private co-op run.' }
    $prior=Get-Content -LiteralPath $resumePath -Raw | ConvertFrom-Json
    if ($prior.source_branch -ne 'coop-c86') { throw 'Only a C86 sweep may be resumed.' }
    # A service interruption can leave 'running' after the approved runner is
    # gone. Retrying still goes through that runner and its normal slot checks.
    $scenariosToRun=@($prior.runs | Where-Object { $_.status -in @('pending','running','blocked-task-stop','blocked-deadline','blocked-disk-space','waiting-runner','stopped','error') })
}
for ($i = 0; $i -lt $scenariosToRun.Count; $i++) {
    $scenario=$scenariosToRun[$i]; $entry=[int]$scenario.entry
    $records += [ordered]@{
        entry = $entry; name = ('sweep-e{0:D2}-s{1}-{2:D3}' -f $entry,$scenario.start,$i)
        status = 'pending'; run = $null; runner_output = (Join-Path $sweepDir "runner-$i.log")
        exit_code = $null; started_utc = $null; finished_utc = $null; error = $null
        start = $scenario.start; level = $rules[$entry].level; policy = $rules[$entry].policy
        load_proof = $scenario.load_proof; world_evidence = (Join-Path $sweepDir "world-$i"); world_status = $null
        seconds = $(if ($scenario.load_proof -eq 'forced-checkpoint-smoke') { $CheckpointSeconds } else { $Seconds })
    }
}
$manifest = [ordered]@{
    schema_version = 1; created_utc = [DateTime]::UtcNow.ToString('o')
    seconds = $Seconds; checkpoint_seconds = $CheckpointSeconds; worktree = $Worktree; face_partner = [bool]$FacePartner; runs = $records
    world_diff = [bool]$WorldDiff; checkpoints = [bool]$Checkpoints; source_commit = $sourceCommit
    source_branch = $sourceBranch; source_dirty = $sourceDirty; exe_sha256 = $exeSha
    protocol = 21; snapshot_schema = 1; fault_profile = 'bad_wifi'; fault_seeds = @(86,86)
    scenarios = $rules; resumed_from = $ResumeManifest
    harness_sha256 = $toolHashes; harness_sources = $harnessDir
}
function Save-Manifest { $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8 }
Save-Manifest
Write-Host "MANIFEST $manifestPath"
# Do not inherit stale death/end/crash/teleport aids from another experiment.
# Changes are process-local and restored even when a runner fails.
$previousDevEnv = @{}
Get-ChildItem Env: | Where-Object Name -like 'KK_DEV_*' | ForEach-Object {
    $previousDevEnv[$_.Name] = $_.Value
}
$failed = $false
try {
    foreach ($key in $previousDevEnv.Keys) { [Environment]::SetEnvironmentVariable($key, $null, 'Process') }
    for ($recordIndex=0; $recordIndex -lt $records.Count -and $recordIndex -lt $MaxRuns; $recordIndex++) {
        $record=$records[$recordIndex]
        if (Test-Path -LiteralPath (Join-Path $sweepDir 'stop-after-current')) { $record.status='blocked-task-stop'; Save-Manifest; break }
        if ($WorldDiff -and (Get-PSDrive -Name F).Free -lt 8GB) { $record.status='blocked-disk-space'; Save-Manifest; break }
        if ([DateTime]::UtcNow.AddSeconds($record.seconds + 30) -gt $deadline) { $record.status='blocked-deadline'; Save-Manifest; break }
        $record.status = 'running'; $record.started_utc = [DateTime]::UtcNow.ToString('o'); Save-Manifest
        $runOutput = New-Object 'System.Collections.Generic.List[string]'
        $envText = "KK_DEV_LAUNCHER_INPUT=wait-home,direct:code:C86ABC,direct:host,session:$($record.entry)|KK_DEV_AUTOSKIP=1|KK_DEV_SCRIPT=0:SKIP/10|KK_DEV_WANDER=1|KK_DEV_CHEATS_ANYWHERE=1|KK_DEV_EMPTY_LOADOUT=1|KK_DEV_TRIG_TRACE=1|KK_DEV_ENEMY_NEAR=1"
        $joinText = 'KK_DEV_LAUNCHER_INPUT=wait-home,direct:code:C86ABC,direct:join,direct:ready|KK_DEV_FACE_ENEMY=16:0.001'
        if ($StoryLayout) { $envText += "|KK_DEV_STORY_LAYOUT=$StoryLayout" }
        if ($Stationary) { $envText = $envText.Replace('|KK_DEV_WANDER=1','') }
        if ($FacePartner) {
            $envText += '|KK_DEV_FACE_PARTNER=90:0.001'
            $joinText = 'KK_DEV_LAUNCHER_INPUT=wait-home,direct:code:C86ABC,direct:join,direct:ready'
        }
        $hostArgs = '--kk_cheats=true|--kk_cheat_machine_gun=true|--kk_cheat_ammo=true|--kk_cheat_healing=true|--kk_no_healing=false|--kk_cheat_one_hit=false|--kk_cheat_revolver=false|--kk_cheat_shotgun=false|--kk_cheat_sniper=false'
        if ($WorldDiff) {
            New-Item -ItemType Directory -Force -Path "$($record.world_evidence)\host","$($record.world_evidence)\guest" | Out-Null
            $scenario="entry-$($record.entry)-start-$($record.start)"
            $evidence=[ordered]@{ entry=$record.entry; start=$record.start; level=$record.level; policy=$record.policy
                load_proof=$record.load_proof; source_commit=$sourceCommit; source_branch=$sourceBranch; source_dirty=$sourceDirty
                exe_sha256=$exeSha; exe_path=$exePath; protocol=21; snapshot_schema=1; fault_profile='bad_wifi'; seconds=$record.seconds
                fault_seeds=@(86,86); port=27080; roles=@('host','guest'); scenario=$scenario; flags=$hostArgs; runner=$runner
                harness_sha256=$toolHashes; harness_sources=$harnessDir }
            $dirtyText=($sourceDirty -join ';')
            $envText += "|KK_DEV_WORLD_FAULT_PROFILE=bad_wifi|KK_DEV_WORLD_DUMP=5,2,36|KK_DEV_WORLD_ROUTE=32|KK_DEV_WORLD_DIR=$($record.world_evidence)\host|KK_DEV_WORLD_COMMIT=$sourceCommit|KK_DEV_WORLD_DIRTY=$dirtyText|KK_DEV_WORLD_EXE_SHA256=$exeSha|KK_DEV_WORLD_SCENARIO=$scenario"
            $joinText += "|KK_DEV_WORLD_DIR=$($record.world_evidence)\guest"
            if ($null -ne $record.start) { $envText += "|KK_DEV_START_POINT=$($record.start)" }
            $evidence['env']=$envText; $evidence['join_env']=$joinText
            $evidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$($record.world_evidence)\evidence.json" -Encoding UTF8
        }
        try {
            # A child shell contains coop-run's exit statements and Add-Type declarations.
            # Do not add an -Entry start: SessionStart from the lobby must drive both loads.
            # Do not pass a final --name= with no value: the native flag parser rejects
            # the guest command line. The private setupdone config has no policy override.
            $attempt=0
            do {
            $attempt++; $runOutput.Clear()
            "runner attempt $attempt" | Add-Content -LiteralPath $record.runner_output -Encoding UTF8
            & $shellExe -NoProfile -ExecutionPolicy Bypass -File $runner -Name $record.name `
                -Seconds $record.seconds -Worktree $Worktree -Two -Launcher -DirectIp -Snap '60' -Env $envText -JoinEnv $joinText `
                -HostExtra $hostArgs -Extra '--kk_launcher=true|--kk_toggle_aim=false|--kk_coop_udp_profile=bad_wifi|--kk_coop_udp_seed=86' 2>&1 |
                ForEach-Object {
                    $line = $_.ToString(); $runOutput.Add($line)
                    $line | Add-Content -LiteralPath $record.runner_output -Encoding UTF8
                    Write-Host $line
                }
            $record.exit_code = $LASTEXITCODE
            $blocked=($runOutput -match '^USER:|^QUIET:|^CRAIG:|^BUSY:') -and -not ($runOutput -match '^(host|join|solo) PID ')
            $retry=$blocked -and $RetryBlockedSeconds -gt 0 -and [DateTime]::UtcNow.AddSeconds($record.seconds+30+$RetryBlockedSeconds) -lt $deadline
            if ($retry) {
                $record.status='waiting-runner'; Save-Manifest
                Write-Host "Waiting $RetryBlockedSeconds seconds for the approved runner."
                Start-Sleep -Seconds $RetryBlockedSeconds
                $record.status='running'; Save-Manifest
            }
            } while ($retry)
            $record.status = 'completed'
            if ($record.exit_code -ne 0 -or -not ($runOutput -match '^RUN ') -or
                -not ($runOutput -contains 'slots released') -or
                ($runOutput -match 'WARNING still running|^USER:|^QUIET:|^CRAIG:|^BUSY:|^CRAIG started')) {
                $record.status = 'stopped'; $failed = $true
            }
        } catch {
            $record.status = 'error'; $record.error = $_.Exception.Message; $failed = $true
            $record.error | Add-Content -LiteralPath $record.runner_output -Encoding UTF8
        } finally {
            foreach ($line in $runOutput) {
                if ($line -match '^RUN (.+)$') { $record.run = $Matches[1].Trim() }
            }
            $record.finished_utc = [DateTime]::UtcNow.ToString('o'); Save-Manifest
        }
        if ($WorldDiff -and $record.run) {
            & $pythonExe -I (Join-Path $harnessDir 'world-sweep-score.py') $record.world_evidence $record.run
            if ($LASTEXITCODE -eq 0) {
                $scored=Get-Content -LiteralPath "$($record.world_evidence)\result.json" -Raw | ConvertFrom-Json
                $record.world_status=$scored.status
                if ($Checkpoints -and $record.load_proof -eq 'chapter-entry-smoke' -and -not $failed) {
                    $nativeStart=@(Get-ChildItem -LiteralPath "$($record.world_evidence)\host" -Filter 'world-g*.json' | Select-Object -First 1 | ForEach-Object {
                        (Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json).identity.start
                    })
                    $checkpointCount=0
                    foreach ($cp in $scored.native_checkpoint_ids) {
                        if ($nativeStart -contains $cp -or $checkpointCount -ge $CheckpointLimit) { continue }
                        $n=$records.Count; $checkpointCount++
                        $records += [ordered]@{ entry=$record.entry; name=('sweep-e{0:D2}-cp{1}-{2:D3}' -f $record.entry,$cp,$n)
                            status='pending'; run=$null; runner_output=(Join-Path $sweepDir "runner-$n.log"); exit_code=$null
                            started_utc=$null; finished_utc=$null; error=$null; start=$cp; level=$record.level; policy=$record.policy
                            load_proof='forced-checkpoint-smoke'; world_evidence=(Join-Path $sweepDir "world-$n"); world_status=$null; seconds=$CheckpointSeconds }
                    }
                    $manifest.runs=$records
                }
            } else { $record.world_status='INVALID-scorer' }
            Save-Manifest
        }
        # Respect owner/quiet/busy signals and never launch again after uncertain cleanup.
        if ($failed) { break }
    }
} finally {
    Get-ChildItem Env: | Where-Object Name -like 'KK_DEV_*' | ForEach-Object {
        [Environment]::SetEnvironmentVariable($_.Name, $null, 'Process')
    }
    foreach ($key in $previousDevEnv.Keys) { [Environment]::SetEnvironmentVariable($key, $previousDevEnv[$key], 'Process') }
    Save-Manifest
    Write-Host "MANIFEST $manifestPath"
}
if ($failed) { throw "Sweep stopped. Inspect $manifestPath and its runner logs before retrying." }




