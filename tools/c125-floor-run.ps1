param([Parameter(Mandatory)][string]$Name,[int]$Entry=1,[int]$Start=3,
      [Parameter(Mandatory)][string]$Point,[ValidateSet('host','guest')][string]$Breaker='host',
      [string]$Preset='kk-dev-vulkan',[int]$Seconds=130,[int]$Seed=125,
      [string]$Source='',[string]$Walk='26:LSUP/2',[string]$ExtraEnv='')
$ErrorActionPreference='Stop'
$worktree=Split-Path $PSScriptRoot -Parent
$state='F:\kk-native-coop\codex\state'
$common="KK_DEV_AUTOSKIP=1|KK_DEV_SCRIPT=0:SKIP/10|KK_DEV_START_POINT=$Start|KK_DEV_CHEATS_ANYWHERE=1|KK_DEV_NPC_HOLD_FIRE=1|KK_DEV_OBJECTS=1|KK_DEV_OBJECT_PASSAGE=1|KK_DEV_OBJECT_FLOOR=10|KK_DEV_OBJECT_FLOOR_POINT=$Point|KK_DEV_OBJECT_STAGE=18"
if ($Source) { $common+="|KK_DEV_OBJECT_SOURCE=$Source" }
if ($ExtraEnv) { $common+='|'+$ExtraEnv }
$operator='|KK_DEV_OBJECT_HIT=20'
$walker="|KK_DEV_OBJECT_HIT=|KK_DEV_SCRIPT=0:SKIP/10,$Walk"
$hostText="KK_DEV_LAUNCHER_INPUT=wait-home,direct:code:C125AB,direct:host,session:$Entry|$common"
$joinText="KK_DEV_LAUNCHER_INPUT=wait-home,direct:code:C125AB,direct:join,direct:ready|$common"
if ($Breaker -eq 'host') { $hostText+=$operator; $joinText+=$walker }
else { $hostText+=$walker; $joinText+=$operator }
$extra="--kk_shaders_prepared=2.0.0-beta.17;spirv,dxil|--kk_setup_again=false|--kk_toggle_aim=false|--kk_coop_udp_profile=bad_wifi|--kk_coop_udp_seed=$Seed"
$fixture=[ordered]@{name=$Name;entry=$Entry;start=$Start;breaker=$Breaker;preset=$Preset;seed=$Seed;
    source_commit=(& git -C $worktree rev-parse HEAD);dirty_paths=@(& git -C $worktree status --short);
    binary_sha256=(Get-FileHash -LiteralPath "$worktree\kk\out\build\$Preset\king_kong.exe").Hash;
    environment=$hostText;join_environment=$joinText;extra=$extra;seconds=$Seconds}
$fixture | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$state\$Name-fixture.json"
& powershell -NoProfile -File F:\kk-native-coop\tools\coop-run.ps1 -Name $Name -Seconds $Seconds -Two -Launcher -DirectIp `
    -Worktree $worktree -Preset $Preset -Snap '85,110' -Env $hostText -JoinEnv $joinText `
    -HostExtra '--kk_cheats=true|--kk_cheat_healing=true|--kk_cheat_one_hit=false' -Extra $extra *> "$state\$Name-run.log"
$result=$LASTEXITCODE
if ($result -ne 0) { Write-Output "$Name exit=$result"; exit $result }
$run=Get-ChildItem -LiteralPath 'F:\kk-native-coop\runs' -Directory -Filter "$Name-*" | Sort-Object Name | Select-Object -Last 1
$audit=[ordered]@{run=$run.FullName;roles=@()}
foreach ($role in 'host','join') {
    $files=@(Get-ChildItem -LiteralPath "$($run.FullName)\$role" -Filter 'run*.log' -Recurse | Sort-Object FullName)
    $lines=@($files | ForEach-Object { Get-Content -LiteralPath $_.FullName })
    $audit.roles+= [ordered]@{role=$role;logs=@($files.FullName);
        native_inputs=@($lines | Select-String 'object probe: native input').Count;
        stages=@($lines | Select-String 'object probe: placed');
        impulses=@($lines | Select-String 'barrier impulse:');
        void_deaths=@($lines | Select-String 'native-ground.*condition=\d->3.*void=1').Count;
        placements=@($lines | Select-String 'co-op placement:');
        passage=@($lines | Select-String 'object passage:');
        failures=@($lines | Select-String 'probe: expired|Access violation|\[error\]')}
}
$copies=@(Get-ChildItem -LiteralPath $run.FullName -Directory -Recurse | Where-Object Name -in @('bin','cache'))
if ($copies) { throw "Run copies remain: $($copies.FullName -join ', ')" }
$audit | ConvertTo-Json -Depth 5 -Compress | Set-Content -LiteralPath "$state\$Name-audit.json"
Write-Output "$Name exit=$result run=$($run.Name)"
exit $result
