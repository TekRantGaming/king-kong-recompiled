param([int]$First=1,[int]$Last=10)
$ErrorActionPreference='Stop'
foreach($index in $First..$Last) {
    $breaker=if($index%2) {'host'} else {'guest'}
    $entry=1;$start=3;$point='319.631,273.888,25';$joinPoint='321.5,273.888,25';$source='A098251F7A73B76D'
    $backend=if($index -le 4) {'vulkan'} else {'d3d12'}
    $frameRate=if($index -le 4) {30} else {120}
    $walk='21.5:LSUP/2,25:LSLEFT/1,28:LSUP/1';$extraEnv='';$seconds=130
    if($index -in @(7,8)) {
        $entry=24;$start=1;$point='30,93,12';$joinPoint='33,93,12';$source='44554B4D8561A02F'
        $walk='23:LSUP/3'
        $extraEnv='KK_DEV_OBJECT_FLOOR_SCAN=1|KK_DEV_OBJECT_FLOOR_STEP=1|KK_DEV_FACE_POINT=22:0.05,28.768,90.871,5.5'
    }
    if($index -ge 9) {
        $entry=4;$start=1;$point='89,99,40';$joinPoint='89,103,40';$source='E288DB6E01C48E47'
        $walk='23:LSUP/2'
        $extraEnv='KK_DEV_FACE_POINT=22:0.05,85.319,99.133,33.0'
    }
    $name="c125-final-$index"
    $driver="F:\kk-native-coop\codex\state\$name-driver.log"
    do {
        $driverArgs=@('-NoProfile','-File',"$PSScriptRoot\c125-floor-run.ps1",'-Name',$name,'-Entry',"$entry",'-Start',"$start",
            '-Point',$point,'-JoinPoint',$joinPoint,'-Source',$source,'-Breaker',$breaker,'-Seed',"$(150+$index)",'-Seconds',"$seconds",
            '-Backend',$backend,'-FrameRate',"$frameRate",'-Walk',$walk,'-FloorAt','15')
        if($extraEnv) {$driverArgs+=@('-ExtraEnv',$extraEnv)}
        & powershell @driverArgs *> $driver
        $code=$LASTEXITCODE
        if($code -in @(3,4)) {Write-Output "$name refused $code";Start-Sleep -Seconds 30}
    } while($code -in @(3,4))
    if($code -ne 0) {throw "$name failed $code; inspect $driver"}
    $audit=Get-Content -LiteralPath "F:\kk-native-coop\codex\state\$name-audit.json" -Raw | ConvertFrom-Json
    $voids=($audit.roles | Measure-Object void_deaths -Sum).Sum
    $invalid=@($audit.roles.impulses | Where-Object {$_ -like '*finite=false*'}).Count
    Write-Output "$name valid=$($audit.valid_crossing) void=$voids invalid-velocity=$invalid"
    if($voids -or $invalid) {throw "$name captured a failure; keep it for investigation"}
}
