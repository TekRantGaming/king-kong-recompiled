param([ValidateSet('before','after')][string]$Phase='before',[int]$Count=10,[int]$First=1,[string]$Indices='',
      [string]$Preset='kk-dev-vulkan',[int]$Seconds=110)
$ErrorActionPreference='Stop'
$cases=if($Indices) {@($Indices -split ',' | ForEach-Object {[int]$_})} else {@($First..($First+$Count-1))}
foreach ($index in $cases) {
    $breaker=if($index%2) {'host'} else {'guest'}
    $name="c125-$Phase-$index"
    $driver="F:\kk-native-coop\codex\state\$name-driver.log"
    $backend='d3d12'; $frameRate=120; $walk='21.5:LSUP/2,25:LSLEFT/1,28:LSUP/1'; $floorAt=15
    $entry=1; $start=3; $point='319.631,273.888,25'; $source='A098251F7A73B76D'; $seed=125+$index; $extraEnv=''; $runSeconds=$Seconds
    if ($Phase -eq 'after') {
        $baseline=Get-Content -LiteralPath "F:\kk-native-coop\codex\state\c125-before-$index-fixture.json" -Raw | ConvertFrom-Json
        $entry=$baseline.entry; $start=$baseline.start; $breaker=$baseline.breaker; $seed=$baseline.seed
        $runSeconds=[Math]::Max($Seconds,[int]$baseline.seconds)
        if($baseline.extra_env) {$extraEnv=$baseline.extra_env}
        $point=if($baseline.environment -match 'KK_DEV_OBJECT_FLOOR_POINT=([^|]+)') {$Matches[1]} else {throw 'Missing baseline floor point'}
        $source=if($baseline.environment -match 'KK_DEV_OBJECT_SOURCE=([^|]+)') {$Matches[1]} else {
            $beforeAudit=Get-Content -LiteralPath "F:\kk-native-coop\codex\state\c125-before-$index-audit.json" -Raw | ConvertFrom-Json
            if($beforeAudit.target) {$beforeAudit.target} else {'0'}
        }
        $backend=if($baseline.extra -match '--native_backend=([^|]+)') {$Matches[1]} else {'vulkan'}
        $frameRate=if($baseline.extra -match '--kk_frame_rate=(\d+)') {[int]$Matches[1]} else {30}
        $floorAt=if($baseline.environment -match 'KK_DEV_OBJECT_FLOOR=(\d+)') {[int]$Matches[1]} else {10}
        $walkerText=if($breaker -eq 'host') {$baseline.join_environment} else {$baseline.environment}
        $walk=(@($walkerText -split '\|' | Where-Object {$_ -like 'KK_DEV_SCRIPT=*'}) | Select-Object -Last 1).Replace('KK_DEV_SCRIPT=0:SKIP/10,','')
    }
    # A refusal never reserves or borrows the other job's slots.
    do {
        $driverArgs=@('-NoProfile','-File',"$PSScriptRoot\c125-floor-run.ps1",'-Name',$name,'-Entry',"$entry",'-Start',"$start",
            '-Point',$point,'-Breaker',$breaker,'-Preset',$Preset,'-Seed',"$seed",'-Seconds',"$runSeconds",
            '-Backend',$backend,'-FrameRate',"$frameRate",'-Walk',$walk,'-FloorAt',"$floorAt")
        if($source) {$driverArgs+=@('-Source',$source)}
        if($extraEnv) {$driverArgs+=@('-ExtraEnv',$extraEnv)}
        & powershell @driverArgs *> $driver
        $code=$LASTEXITCODE
        if ($code -in @(3,4)) { Write-Output "$name refused $code"; Start-Sleep -Seconds 30 }
    } while($code -in @(3,4))
    if ($code -ne 0) { throw "$name failed $code; inspect $driver" }
    Write-Output "$name completed"
}
