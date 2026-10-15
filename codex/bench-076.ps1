param([switch]$ResumeAfterFirstBefore,[int]$StartRepeat=1)
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path "$PSScriptRoot/..").Path
$out="$workspace\out\task076"
$env:PATH='F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc\bin\x64;'+$env:PATH
$env:NR_BENCH='1'; $env:NR_DIAGNOSTICS='0'
$executables=@("$out\baseline\backend\tests\nr_game_upscale_tests.exe", "$workspace\kk\out\build\kk-dev-vulkan\native-renderer\backend\tests\nr_game_upscale_tests.exe")
function Busy-Processes {
 @(Get-CimInstance Win32_Process | Where-Object {$_.Name -match '^(king_kong|cmake|ninja|clang.*|cl|lld.*|link|MSBuild)\.exe$'})
}
for($repeat=$StartRepeat;$repeat -le 3;$repeat++) {
 for($side=0;$side -lt 2;$side++) {
  if($ResumeAfterFirstBefore -and $repeat -eq 1 -and $side -eq 0){continue}
  while($busy=Busy-Processes) { Write-Output "WAIT no-game/no-build: $($busy.Name -join ', ')";Start-Sleep -Seconds 30 }
  $name=if($side -eq 0){'before'}else{'after'}
  $log="$out\bench-$name-$repeat.log"
  foreach($old in @($log,"$out\bench-$name-$repeat.stderr.log","$out\bench-$name-$repeat-processes.json")) {if(Test-Path $old){Move-Item -LiteralPath $old -Destination ($old+'.attempt-'+(Get-Date -Format HHmmssfff))}}
  @(Get-CimInstance Win32_Process | Select-Object Name,ProcessId,ParentProcessId,ExecutablePath) | ConvertTo-Json | Set-Content "$out\bench-$name-$repeat-processes.json"
  $p=Start-Process $executables[$side] -ArgumentList 'Bench_CpuPerDraw' -WorkingDirectory (Split-Path $executables[$side]) -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError "$out\bench-$name-$repeat.stderr.log"
  $processHandle=$p.Handle
  $contaminated=$false
  try {
   while(!$p.HasExited) {
    if($busy=Busy-Processes){$busy | Select-Object Name,ProcessId,CommandLine | ConvertTo-Json | Set-Content "$out\bench-$name-$repeat-contaminants.json";$contaminated=$true;Stop-Process -Id $p.Id;$p.WaitForExit();break}
    Start-Sleep -Milliseconds 500
   }
  }finally{if(!$p.HasExited){Stop-Process -Id $p.Id;$p.WaitForExit()}}
  if($contaminated){throw "Benchmark contaminated: $name $repeat"}
  $p.WaitForExit()
  if($p.ExitCode -ne 0 -or !(Select-String -LiteralPath $log -SimpleMatch '1 tests, 0 failed checks')){throw "Benchmark failed: $name $repeat exit $($p.ExitCode)"}
  Write-Output "PASS $name $repeat"
 }
}
