# Normal beta has no developer input or fault injector. Use its existing
# Direct IP resume path to verify the shipping code connects and loads both
# worlds. This is a smoke, not a barrier crossing or bad wifi claim.
param([string]$Name='c125-beta-native',[int]$Entry=1,[int]$Seconds=150)
$ErrorActionPreference='Stop'
$worktree=Split-Path $PSScriptRoot -Parent
$state='F:\kk-native-coop\codex\state'
$cache=Get-Content -LiteralPath "$worktree\kk\out\build\kk-beta\CMakeCache.txt"
if(-not ($cache -contains 'KK_DEV_TOOLS:BOOL=OFF')) {throw 'This smoke requires normal beta with developer aids off'}
$cachedVersion=@($cache | Where-Object {$_ -match '^KK_VERSION:[^=]+=(.*)$'})
if($cachedVersion) {$version=($cachedVersion[0] -split '=',2)[1]}
else {
    $versionMatch=Select-String -LiteralPath "$worktree\kk\CMakeLists.txt" -Pattern '^\s*set\(KK_VERSION\s+"([^"]+)"\)'
    $version=$versionMatch.Matches[0].Groups[1].Value
}
$protocolMatch=Select-String -LiteralPath "$worktree\kk\src\coop_protocol.h" -Pattern 'constexpr uint16_t kProtocolVersion = (\d+);'
$protocol=$protocolMatch.Matches[0].Groups[1].Value
$token='00000000c1250033'
$written=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$inputFiles=@()
foreach($role in 'host','guest') {
    $file="coop-resume-$role-$token.txt"
    $content=@('kk_coop_resume=1',"role=$role","token=$token",'transport=direct',"chapter=$Entry",'continue=0','slot=-1',
        'host_id=0','guest_id=0','port=33225','address=127.0.0.1','code=C125NT','partner=Peer',"protocol=$protocol", "version=$version","written=$written")
    $path="F:\kk-native-coop\userdata\$file"
    Set-Content -LiteralPath $path -Value $content -Encoding ASCII
    $inputFiles+=$path
}
$extra='--kk_shaders_prepared=2.0.0-beta.17;spirv,dxil|--kk_setup_again=false|--native_backend=d3d12|--kk_frame_rate=120'
$fixture=[ordered]@{name=$Name;mode='native_beta_smoke';entry=$Entry;breaker='host';preset='kk-beta';seconds=$Seconds;
    source_commit=(& git -C $worktree rev-parse HEAD);dirty_paths=@(& git -C $worktree status --short);
    source_files=@('kk/src/coop_objects_game.cpp','kk/src/coop_protocol.cpp','kk/src/coop_protocol.h','kk/src/coop_enemies_game.cpp','kk/src/coop_barrier.h','kk/src/coop_player_contact.h','kk/src/coop_partner_body.cpp','kk/src/coop_partner_body.h' | ForEach-Object {
        [ordered]@{path=$_;sha256=(Get-FileHash -LiteralPath "$worktree\$_").Hash}
    });
    binary_sha256=(Get-FileHash -LiteralPath "$worktree\kk\out\build\kk-beta\king_kong.exe").Hash;
    developer_aids=$false;fault_injection=$false;input_files=$inputFiles;extra=$extra}
$fixture | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$state\$Name-fixture.json"
$ErrorActionPreference='Continue'
& powershell -NoProfile -File F:\kk-native-coop\tools\coop-run.ps1 -Name $Name -Seconds $Seconds -Two -Launcher -DirectIp `
    -Worktree $worktree -Preset kk-beta -Snap '80,120' -HostExtra "--kk_cheats=true|--kk_cheat_healing=true|--kk_coop_resume=coop-resume-host-$token.txt" `
    -JoinExtra "--kk_coop_resume=coop-resume-guest-$token.txt" -Extra $extra *> "$state\$Name-run.log"
$result=$LASTEXITCODE
$ErrorActionPreference='Stop'
if($result -in @(3,4)) {Write-Output "$Name refused $result";exit $result}
& powershell -NoProfile -File "$PSScriptRoot\c125-floor-run.ps1" -Name $Name -Point '0,0,0' -AuditOnly *> "$state\$Name-audit-driver.log"
if($LASTEXITCODE -ne 0) {throw 'Native beta cleanup or log audit failed'}
$audit=Get-Content -LiteralPath "$state\$Name-audit.json" -Raw | ConvertFrom-Json
if(@($audit.roles | Where-Object { -not $_.load_released -or $_.game_crashes }).Count) {throw 'Both native beta worlds did not reach the shared load barrier'}
Write-Output "$Name normal beta both worlds loaded, developer aids off, runner exit $result"
exit 0
