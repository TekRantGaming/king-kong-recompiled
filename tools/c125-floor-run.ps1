param([Parameter(Mandatory)][string]$Name,[int]$Entry=1,[int]$Start=3,
      [Parameter(Mandatory)][string]$Point,[ValidateSet('host','guest')][string]$Breaker='host',
      [string]$Preset='kk-dev-vulkan',[int]$Seconds=130,[int]$Seed=125,
      [ValidateSet('vulkan','d3d12')][string]$Backend='d3d12',[int]$FrameRate=120,
      [int]$FloorAt=15,[switch]$AuditOnly,
      [string]$Source='',[string]$Walk='21.5:LSUP/2,25:LSLEFT/1,28:LSUP/1',[string]$ExtraEnv='',[string]$JoinPoint='',
      [switch]$NativeResume)
$ErrorActionPreference='Stop'
$worktree=Split-Path $PSScriptRoot -Parent
$state='F:\kk-native-coop\codex\state'
$common="KK_DEV_AUTOSKIP=1|KK_DEV_SCRIPT=0:SKIP/10|KK_DEV_START_POINT=$Start|KK_DEV_CHEATS_ANYWHERE=1|KK_DEV_NPC_HOLD_FIRE=1|KK_DEV_OBJECTS=1|KK_DEV_OBJECT_PASSAGE=1|KK_DEV_OBJECT_FLOOR=$FloorAt|KK_DEV_OBJECT_FLOOR_POINT=$Point|KK_DEV_OBJECT_STAGE=18"
if ($Source) { $common+="|KK_DEV_OBJECT_SOURCE=$Source" }
if ($ExtraEnv) { $common+='|'+$ExtraEnv }
$operator='|KK_DEV_OBJECT_HIT=20'
$walker="|KK_DEV_OBJECT_HIT=|KK_DEV_SCRIPT=0:SKIP/10,$Walk"
$hostText="KK_DEV_LAUNCHER_INPUT=wait-home,direct:code:C125AB,direct:host,session:$Entry|$common"
$joinText="KK_DEV_LAUNCHER_INPUT=wait-home,direct:code:C125AB,direct:join,direct:ready|$common"
if ($JoinPoint) {$joinText=$joinText.Replace("KK_DEV_OBJECT_FLOOR_POINT=$Point","KK_DEV_OBJECT_FLOOR_POINT=$JoinPoint")}
if ($Breaker -eq 'host') { $hostText+=$operator; $joinText+=$walker }
else { $hostText+=$walker; $joinText+=$operator }
$hostExtra='--kk_cheats=true|--kk_cheat_healing=true|--kk_cheat_one_hit=false';$joinExtra=''
if ($NativeResume -and -not $AuditOnly) {
    # Exercise the game's existing resume entry without any scripted lobby actions.
    $versionMatch=Select-String -LiteralPath "$worktree\kk\CMakeLists.txt" -Pattern '^\s*set\(KK_VERSION\s+"([^"]+)"\)'
    $version=$versionMatch.Matches[0].Groups[1].Value
    $protocolMatch=Select-String -LiteralPath "$worktree\kk\src\coop_protocol.h" -Pattern 'constexpr uint16_t kProtocolVersion = (\d+);'
    $protocol=$protocolMatch.Matches[0].Groups[1].Value
    $written=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds();$token='00000000c1250033'
    foreach($role in 'host','guest') {
        $file="coop-resume-$role-$token.txt"
        $content=@('kk_coop_resume=1',"role=$role","token=$token",'transport=direct',"chapter=$Entry",'continue=0','slot=-1',
            'host_id=0','guest_id=0','port=33225','address=127.0.0.1','code=C125AB','partner=Peer',"protocol=$protocol", "version=$version","written=$written")
        Set-Content -LiteralPath "F:\kk-native-coop\userdata\$file" -Value $content -Encoding ASCII
        if($role -eq 'host') {$hostExtra+="|--kk_coop_resume=$file"} else {$joinExtra="--kk_coop_resume=$file"}
    }
    $hostText=$hostText -replace 'KK_DEV_LAUNCHER_INPUT=[^|]*\|','KK_DEV_LAUNCHER_INPUT=|'
    $joinText=$joinText -replace 'KK_DEV_LAUNCHER_INPUT=[^|]*\|','KK_DEV_LAUNCHER_INPUT=|'
}
$extra="--kk_shaders_prepared=2.0.0-beta.17;spirv,dxil|--kk_setup_again=false|--kk_toggle_aim=false|--kk_coop_udp_profile=bad_wifi|--kk_coop_udp_seed=$Seed|--native_backend=$Backend|--kk_frame_rate=$FrameRate"
if ($AuditOnly) {
    $fixture=Get-Content -LiteralPath "$state\$Name-fixture.json" -Raw | ConvertFrom-Json
    $Breaker=$fixture.breaker; $result=0
} else {
  $fixture=[ordered]@{name=$Name;entry=$Entry;start=$Start;breaker=$Breaker;preset=$Preset;seed=$Seed;
    point=$Point;join_point=$JoinPoint;object_source=$Source;walk=$Walk;backend=$Backend;frame_rate=$FrameRate;floor_at=$FloorAt;extra_env=$ExtraEnv;native_resume=[bool]$NativeResume;
    source_commit=(& git -C $worktree rev-parse HEAD);dirty_paths=@(& git -C $worktree status --short);
    developer_tools=@(Select-String -LiteralPath "$worktree\kk\out\build\$Preset\CMakeCache.txt" -Pattern '^KK_DEV_TOOLS:BOOL=' | ForEach-Object {$_.Line});
    binary_sha256=(Get-FileHash -LiteralPath "$worktree\kk\out\build\$Preset\king_kong.exe").Hash;
    environment=$hostText;join_environment=$joinText;extra=$extra;seconds=$Seconds}
$fixture | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$state\$Name-fixture.json"
  $ErrorActionPreference='Continue'
& powershell -NoProfile -File F:\kk-native-coop\tools\coop-run.ps1 -Name $Name -Seconds $Seconds -Two -Launcher -DirectIp `
    -Worktree $worktree -Preset $Preset -Snap '85,110' -Env $hostText -JoinEnv $joinText `
    -HostExtra $hostExtra -JoinExtra $joinExtra -Extra $extra *> "$state\$Name-run.log"
  $result=$LASTEXITCODE
  $ErrorActionPreference='Stop'
}
if ($result -in @(3,4)) { Write-Output "$Name exit=$result"; exit $result }
$run=Get-ChildItem -LiteralPath 'F:\kk-native-coop\runs' -Directory -Filter "$Name-*" | Sort-Object Name | Select-Object -Last 1
if (-not $run) { throw "$Name has no run folder, runner exit $result" }
$runnerResult=$result
# The runner can finish stopping the games before Windows releases dxcompiler.
# Retry only its existing, bounded private copy cleanup entry, never deletion
# of a computed path here and never a new launch while copies remain.
for($attempt=0;$attempt -lt 10;++$attempt) {
    $copies=@(Get-ChildItem -LiteralPath $run.FullName -Directory -Recurse | Where-Object Name -in @('bin','cache'))
    if(-not $copies) {break}
    $ErrorActionPreference='Continue'
    & powershell -NoProfile -File F:\kk-native-coop\tools\coop-run.ps1 -Name $Name -ReapRun $run.FullName `
        -ReapOwner "coop-$Name cleanup" *> "$state\$Name-cleanup-$attempt.log"
    $ErrorActionPreference='Stop'
    if($attempt -lt 9) {Start-Sleep -Seconds 10}
}
$copies=@(Get-ChildItem -LiteralPath $run.FullName -Directory -Recurse | Where-Object Name -in @('bin','cache'))
if($copies) {throw "Run copies remain: $($copies.FullName -join ', ')"}
if($result -ne 0 -and (Select-String -LiteralPath "$state\$Name-run.log" -Pattern 'cleanup retry' -Quiet)) {$result=0}
if($result -ne 0) {Write-Output "$Name exit=$result";exit $result}
$audit=[ordered]@{run=$run.FullName;runner_exit_code=$runnerResult;roles=@()}
function LogLines($lines,$pattern) { @($lines | Select-String -Pattern $pattern | ForEach-Object {$_.Line}) }
foreach ($role in 'host','join') {
    $files=@(Get-ChildItem -LiteralPath "$($run.FullName)\$role" -Filter 'run*.log' -Recurse | Sort-Object FullName)
    $lines=@($files | ForEach-Object { Get-Content -LiteralPath $_.FullName })
    $audit.roles+= [ordered]@{role=$role;logs=@($files.FullName);
        native_inputs=@(LogLines $lines 'object probe: native input').Count;
        inputs=@(LogLines $lines 'object probe: native input');
        stages=@(LogLines $lines 'object probe: placed');
        impulses=@(LogLines $lines 'barrier impulse:');
        void_deaths=@(LogLines $lines 'native-ground.*condition=\d->3.*void=1').Count;
        deaths=@(LogLines $lines 'native-ground.*condition=\d->3.*void=1');
        void_flags=@(LogLines $lines 'COOP HEALTH void-probe').Count;
        rescues=@(LogLines $lines 'co-op placement: void under Jack').Count;
        placements=@(LogLines $lines 'co-op placement:');
        passage=@(LogLines $lines 'object passage:');
        load_released=@(LogLines $lines 'load barrier released').Count;
        consequences=@(LogLines $lines 'barrier consequences:');
        game_crashes=@(LogLines $lines 'Access violation|unhandled exception|\[critical\]').Count;
        session_ends=@(LogLines $lines 'KK co-op: ended');
        failures=@(LogLines $lines 'probe: expired|Access violation|\[error\]')}
}
$operatorRole=if ($Breaker -eq 'host') {0} else {1}
$walkerRole=1-$operatorRole
$nativeInput=@($audit.roles[$operatorRole].inputs | Sort-Object | Select-Object -First 1)
$target=if($nativeInput -and $nativeInput[0] -match 'id=([0-9A-F]+)') {$Matches[1]} else {''}
$firstCross=@($null,$null)
foreach($roleIndex in 0,1) {
    $approach=0
    $stage=@($audit.roles[$roleIndex].stages | Where-Object {$_ -like "*id=$target *"} | Sort-Object | Select-Object -Last 1)
    $stagedAt=if($stage) {$stage[0].Substring(0,24)} else {''}
    foreach($line in ($audit.roles[$roleIndex].passage | Sort-Object)) {
        if($target -and $stagedAt -and $line.Substring(0,24) -ge $stagedAt -and $line -match "id=$target .*signed=([-\d.]+).*condition=([12])") {
            $signed=[double]::Parse($Matches[1],[Globalization.CultureInfo]::InvariantCulture)
            if(-not $approach -and [Math]::Abs($signed) -gt .25) {$approach=[Math]::Sign($signed)}
            if($approach -and $signed*$approach -lt -.25) {$firstCross[$roleIndex]=$line;break}
        }
    }
}
$firstInput=if($nativeInput) {$nativeInput[0].Substring(0,24)} else {''}
$crossed=$firstCross[$walkerRole] -and $firstCross[$walkerRole].Substring(0,24) -gt $firstInput
$partnerFirst=$crossed -and (-not $firstCross[$operatorRole] -or $firstCross[$walkerRole] -lt $firstCross[$operatorRole])
$earlyDeath=@($audit.roles | ForEach-Object {$_.deaths} | Where-Object {$firstInput -and $_.Substring(0,24) -lt $firstInput}).Count
$earlyEnd=@($audit.roles | ForEach-Object {$_.session_ends} | Where-Object {
    $firstInput -and $_.Substring(0,24) -gt $firstInput -and
    ([datetime]::ParseExact($_.Substring(1,23),'yyyy-MM-dd HH:mm:ss.fff',[Globalization.CultureInfo]::InvariantCulture)-
     [datetime]::ParseExact($firstInput.Substring(1,23),'yyyy-MM-dd HH:mm:ss.fff',[Globalization.CultureInfo]::InvariantCulture)).TotalSeconds -lt 30
}).Count
$audit['target']=$target
$audit['first_cross']=$firstCross
$audit['valid_crossing']=[bool]($nativeInput -and $partnerFirst -and -not $earlyDeath -and -not $earlyEnd -and $audit.roles[0].load_released -and $audit.roles[1].load_released -and $audit.roles[0].impulses.Count -and $audit.roles[1].impulses.Count -and -not $audit.roles[0].game_crashes -and -not $audit.roles[1].game_crashes)
$copies=@(Get-ChildItem -LiteralPath $run.FullName -Directory -Recurse | Where-Object Name -in @('bin','cache'))
if ($copies) { throw "Run copies remain: $($copies.FullName -join ', ')" }
$audit | ConvertTo-Json -Depth 5 -Compress | Set-Content -LiteralPath "$state\$Name-audit.json"
Write-Output "$Name exit=$result run=$($run.Name)"
exit $result
