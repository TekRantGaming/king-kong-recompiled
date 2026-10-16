param([string]$JobList, [double]$MaxMinutes=29)
# Task 147: performance runs inside the quiet window, taken with the shared codex\game-slot.ps1 -Perf
# (perf-quiet + both slots, no builds running). Runs as many jobs as fit in $MaxMinutes, then releases.
# Jobs are run-147.ps1 argument strings separated by '|'. Prints REMAINING <job> for those that did not fit.
$ErrorActionPreference='Stop'
$Jobs=@($JobList -split [regex]::Escape('|') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
foreach($j in $Jobs){ if($j -notmatch '^-Mode (route|chapter) '){throw "bad job: $j"} }
if(!$Jobs.Count){throw 'no jobs'}
$workspace='F:\KK-native-renderer\wt\bench-steady'
$slot='F:\KK-native-renderer\codex\game-slot.ps1'
$log="$workspace/kk/out/task147/quiet-windows.log"
function Note($text){ $line="$(Get-Date -Format o) $text"; Add-Content -LiteralPath $log -Value $line; Write-Output $line }
function Minutes($job){ if($job -match '-Mode route'){ if($job -match '-Passes (\d)'){1.2+3.2*[int]$matches[1]}else{4.5} } else {1.6} }
$claimed=$false
for($i=0; $i -lt 6 -and !$claimed; $i++) {
 $r=& powershell -NoProfile -File $slot -Job claude-bench147 -Perf | Select-Object -Last 1
 Note "game-slot: $r"
 if($r -like 'USER*'){throw 'USER LOCK: code-only work'}
 $claimed=$r -like 'CLAIMED*'
}
if(!$claimed){ & powershell -NoProfile -File $slot -Job claude-bench147 -Release | Out-Null; foreach($j in $Jobs){"REMAINING $j"}; exit 1 }
$window=Get-Date; $done=0
try {
 foreach($job in $Jobs) {
  if(((Get-Date)-$window).TotalMinutes + (Minutes $job) -gt $MaxMinutes) {break}
  $busy=@(Get-Process clang,clang++,clang-20,ninja,lld-link,ld.lld -ErrorAction SilentlyContinue).Count
  Note "job start (build processes $busy): $job"
  $argsList=@('-NoProfile','-ExecutionPolicy','Bypass','-File',"$workspace/codex/run-147.ps1") + ($job -split ' ' | Where-Object {$_}) + '-NoLock'
  & powershell @argsList 2>&1 | Where-Object {"$_" -match 'COMPLETE|LAUNCHED|incomplete|complete,|cancelled|Exception|rror'} | ForEach-Object { "$_" }
  Note "job end: $job"
  $done++
 }
} finally {
 $r=& powershell -NoProfile -File $slot -Job claude-bench147 -Release | Select-Object -Last 1
 Note "quiet window $r after $([int]((Get-Date)-$window).TotalMinutes) min"
}
for($k=$done; $k -lt $Jobs.Count; $k++){ "REMAINING $($Jobs[$k])" }
