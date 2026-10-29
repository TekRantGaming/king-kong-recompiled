# Task C125 control only. Restore the old relay for the corrected walking
# fixtures, retaining the public native floor query. Always restore all fixed
# source bytes and rebuild before returning. No checkout, merge or deletion.
param([string]$DeadlineUtc='2026-10-29T03:50:00Z')
$ErrorActionPreference='Stop'
$worktree=Split-Path $PSScriptRoot -Parent
$state='F:\kk-native-coop\codex\state'
$backup="$state\C125-baseline2-backup"
$deadline=[datetime]::Parse($DeadlineUtc).ToUniversalTime()
$paths=@(& git -C $worktree diff --name-only 66fd9031 0688c2fd | Where-Object {
    $_ -like 'kk/*' -and $_ -ne 'kk/src/coop_barrier.h'
})
if($paths.Count -ne 12) {throw 'The baseline source set changed; inspect before running'}
if(Test-Path -LiteralPath "$backup\manifest.json") {throw 'A control backup already exists; preserve it and inspect'}
$fixedObjects=Get-Content -LiteralPath "$worktree\kk\src\coop_objects_game.cpp" -Raw
$floorPattern='(?s)(void FloorProbe\(.*?\r?\n})\r?\nvoid Probes\('
$floorMatch=[regex]::Match($fixedObjects,$floorPattern)
if(-not $floorMatch.Success -or $floorMatch.Groups[1].Value -notlike '*__imp__sub_82403E38*') {throw 'No fixed public floor probe'}
$fixedFloor=$floorMatch.Groups[1].Value
$manifest=@()
foreach($relative in $paths) {
    $target=[IO.Path]::GetFullPath("$worktree\$relative")
    if(-not $target.StartsWith($worktree+'\',[StringComparison]::OrdinalIgnoreCase)) {throw 'Source escaped the task worktree'}
    $copy="$backup\$relative"
    [void][IO.Directory]::CreateDirectory((Split-Path $copy -Parent))
    Copy-Item -LiteralPath $target -Destination $copy
    $manifest+= [ordered]@{path=$relative;sha256=(Get-FileHash -LiteralPath $target).Hash}
}
$manifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath "$backup\manifest.json"
$utf8=[Text.UTF8Encoding]::new($false)
$failure=$null
try {
    foreach($relative in $paths) {
        $old=@(& git -C $worktree show "66fd9031:$relative")
        if($LASTEXITCODE -ne 0) {throw "No baseline file $relative"}
        [IO.File]::WriteAllText("$worktree\$relative",($old -join "`n")+"`n",$utf8)
    }
    $objects=Get-Content -LiteralPath "$worktree\kk\src\coop_objects_game.cpp" -Raw
    if(-not [regex]::IsMatch($objects,$floorPattern)) {throw 'No old floor probe to replace'}
    $objects=[regex]::Replace($objects,$floorPattern,($fixedFloor+"`nvoid Probes("))
    $objects=$objects.Replace('REX_EXTERN(__imp__sub_828007B0);',"REX_EXTERN(__imp__sub_828007B0);`nREX_EXTERN(__imp__sub_82403E38);")
    [IO.File]::WriteAllText("$worktree\kk\src\coop_objects_game.cpp",$objects,$utf8)
    & git -C $worktree diff -- $paths | Set-Content -LiteralPath "$state\C125-baseline-floor-control.patch"
    $ErrorActionPreference='Continue'
    cmd /c "F:\kk-native-coop\tools\kkbuild.bat kk-dev-vulkan $worktree" *> "$state\C125-build-baseline-control.log"
    $code=$LASTEXITCODE
    $ErrorActionPreference='Stop'
    if($code) {throw "Baseline build failed $code"}
    foreach($index in 1..4) {
        $breaker=if($index%2) {'host'} else {'guest'}
        $name="c125-control-old-$index"
        if($index -le 2) {
            $entry=24;$point='30,93,12';$joinPoint='33,93,12';$source='44554B4D8561A02F';$fps=120
            $extraEnv='KK_DEV_OBJECT_FLOOR_SCAN=1|KK_DEV_OBJECT_FLOOR_STEP=1|KK_DEV_FACE_POINT=22:0.05,25.0,89.5,5.5'
            $walk='23:LSUP/3'
        } else {
            $entry=4;$point='80,103,40';$joinPoint='80,100,40';$source='E288DB6E01C48E47';$fps=30
            $extraEnv='KK_DEV_FACE_POINT=22:0.05,92.0,99.133,33.0';$walk='23:LSUP/2'
        }
        do {
            if([datetime]::UtcNow -ge $deadline) {throw 'Control deadline reached; restore fixed build'}
            & powershell -NoProfile -File "$PSScriptRoot\c125-floor-run.ps1" -Name $name -Entry $entry -Start 1 `
                -Point $point -JoinPoint $joinPoint -Source $source -Breaker $breaker -Seed (170+$index) -Seconds 130 `
                -Backend d3d12 -FrameRate $fps -Walk $walk -FloorAt 15 -ExtraEnv $extraEnv -NativeResume `
                -ProofVariant baseline_native_floor *> "$state\$name-driver.log"
            $code=$LASTEXITCODE
            if($code -in @(3,4)) {Write-Output "$name refused $code";Start-Sleep -Seconds 30}
        } while($code -in @(3,4))
        if($code) {throw "$name failed $code"}
        $audit=Get-Content -LiteralPath "$state\$name-audit.json" -Raw|ConvertFrom-Json
        Write-Output "$name valid=$($audit.valid_crossing) void=$(($audit.roles|Measure-Object void_deaths -Sum).Sum) invalid=$(@($audit.roles.impulses|Where-Object {$_ -like '*finite=false*'}).Count)"
    }
} catch {$failure=$_}
finally {
    foreach($row in $manifest) {
        Copy-Item -LiteralPath "$backup\$($row.path)" -Destination "$worktree\$($row.path)"
        # Copy-Item preserves an older source timestamp. Advance it so Ninja
        # cannot retain an object compiled from the temporary old relay.
        [IO.File]::SetLastWriteTimeUtc("$worktree\$($row.path)",[datetime]::UtcNow)
        if((Get-FileHash -LiteralPath "$worktree\$($row.path)").Hash -ne $row.sha256) {throw "Fixed source restore failed $($row.path)"}
    }
    Write-Output 'All fixed source bytes restored'
    $ErrorActionPreference='Continue'
    cmd /c "F:\kk-native-coop\tools\kkbuild.bat kk-dev-vulkan $worktree" *> "$state\C125-build-restored-fixed.log"
    $restoreCode=$LASTEXITCODE
    $ErrorActionPreference='Stop'
    if($restoreCode) {throw "Restored build failed $restoreCode"}
    $env:PATH="$worktree\kk\out\build\kk-dev-vulkan;"+$env:PATH
    $ErrorActionPreference='Continue'
    & ctest --test-dir "$worktree\kk\out\build\kk-dev-vulkan" -R coop *> "$state\C125-tests-restored-fixed.log"
    $testCode=$LASTEXITCODE
    $ErrorActionPreference='Stop'
    if($testCode) {throw 'Restored co op tests failed'}
    Write-Output 'Restored fixed build and co op tests passed'
}
if($failure) {throw $failure}
