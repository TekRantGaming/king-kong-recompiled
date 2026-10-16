param([string[]]$Jobs, [double]$MaxMinutes=29)
# Task 147: performance runs inside the owner's quiet window (GAME-RULES, 16 Oct): runs\perf-quiet,
# both game slots, no clang/ninja/lld running. Runs as many of $Jobs as fit in $MaxMinutes, then
# releases everything. Each job is an argument string for run-147.ps1 (it is run with -NoLock).
# Prints REMAINING <job> for each job that did not fit.
$ErrorActionPreference='Stop'
$workspace='F:\KK-native-renderer\wt\bench-steady'
. "$workspace/codex/game-slot.ps1"
$task='claude-bench147'
$quiet='F:\KK-native-renderer\runs\perf-quiet'
$slot1='F:\KK-native-renderer\runs\codex-game.lock'; $slot2='F:\KK-native-renderer\runs\codex-game-2.lock'
$log="$workspace/kk/out/task147/quiet-windows.log"
function Note($text){ $line="$(Get-Date -Format o) $text"; Add-Content -LiteralPath $log -Value $line; Write-Output $line }
function Minutes($job){ if($job -match '-Mode route'){ if($job -match '-Passes (\d)'){1.2+3.2*[int]$matches[1]}else{4.5} } else {1.6} }
if((Read-GameSlotState $slot1).StartsWith('user')) {throw 'USER LOCK: no launch'}
# 1. The quiet window file: only if nobody else has it.
$token="$task $(Get-Date -Format o)"
while($true) {
 try { $s=[IO.File]::Open($quiet,'CreateNew','Write','Read'); $b=[Text.Encoding]::ASCII.GetBytes("$token`r`n"); $s.Write($b,0,$b.Length); $s.Dispose(); break }
 catch [IO.IOException] { Start-Sleep -Seconds 5 }
}
Start-Sleep -Seconds 2
if((Get-Content -LiteralPath $quiet -Raw).Trim() -ne $token){throw 'perf-quiet was not ours'}
Note "quiet window taken"
$g1=$null; $g2=$null; $lastGame=Get-Date; $started=Get-Date
try {
 # 2. Both slots (others finish their current game; a slot left behind with no game for 20 min is stale).
 while(!$g1 -or !$g2) {
  if((Read-GameSlotState $slot1).StartsWith('user')) {throw 'USER LOCK: no launch'}
  if(@(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'").Count){$lastGame=Get-Date}
  if(!$g1){ $c=Try-ClaimGameSlot -Task $task -LockPath $slot1 -RequireBackgroundHelper; if($c.Guard){$g1=$c.Guard} }
  if(!$g2){ $c=Try-ClaimGameSlot -Task $task -LockPath $slot2 -RequireBackgroundHelper; if($c.Guard){$g2=$c.Guard} }
  if((!$g1 -or !$g2) -and ((Get-Date)-$lastGame).TotalMinutes -gt 20) {
   foreach($p in @($slot1,$slot2)) {
    $st=Read-GameSlotState $p
    if($st -ne 'free' -and !$st.StartsWith('user') -and !$st.StartsWith($task)) { Set-Content -LiteralPath $p -Value 'free' -Encoding ASCII; Note "stale slot $p ($st): set free" }
   }
  }
  if(!$g1 -or !$g2){Start-Sleep -Seconds 3}
 }
 Start-Sleep -Seconds 2
 if((Read-GameSlotState $slot1) -ne $g1.Token -or (Read-GameSlotState $slot2) -ne $g2.Token){throw 'Lost a slot race'}
 Note "both slots taken after $([int]((Get-Date)-$started).TotalSeconds) s"
 # 3. No compiler or linker running (other agents' builds finishing).
 $waited=Get-Date
 while(@(Get-Process clang,clang++,clang-cl,ninja,lld,lld-link,ld.lld -ErrorAction SilentlyContinue).Count) { Start-Sleep -Seconds 5 }
 Note "no build processes after $([int]((Get-Date)-$waited).TotalSeconds) s"
 $window=Get-Date
 $remaining=[Collections.Generic.List[string]]::new($Jobs)
 while($remaining.Count) {
  $job=$remaining[0]
  if(((Get-Date)-$window).TotalMinutes + (Minutes $job) -gt $MaxMinutes) {break}
  $remaining.RemoveAt(0)
  $busy=@(Get-Process clang,clang++,clang-cl,ninja,lld,lld-link,ld.lld -ErrorAction SilentlyContinue).Count
  Note "job start (build processes $busy): $job"
  $argsList=@('-NoProfile','-ExecutionPolicy','Bypass','-File',"$workspace/codex/run-147.ps1") + ($job -split ' ' | Where-Object {$_}) + '-NoLock'
  & powershell @argsList 2>&1 | Where-Object {$_ -notmatch 'Waiting for game slot'} | ForEach-Object { "$_" }
  Note "job end: $job"
 }
 foreach($j in $remaining){ Write-Output "REMAINING $j" }
} finally {
 if($g2){Release-GameSlot $g2}
 if($g1){Release-GameSlot $g1}
 if(Test-Path -LiteralPath $quiet) { if((Get-Content -LiteralPath $quiet -Raw).Trim() -eq $token){Remove-Item -LiteralPath $quiet} }
 Note "quiet window released after $([int]((Get-Date)-$started).TotalMinutes) min"
}
