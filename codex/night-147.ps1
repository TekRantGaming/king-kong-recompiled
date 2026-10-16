# Task 147 night batch (after 22:00): runs unattended, one quiet window at a time (max 29 min each,
# released between windows), until every job has run. Results land in runs\task147-*.
#   powershell -ExecutionPolicy Bypass -File F:\KK-native-renderer\wt\bench-steady\codex\night-147.ps1
# 1. Two standard 3-pass route runs on the production build (run-to-run check of the medians).
# 2. Second repetition of the 3840x2160 chapter-start comparison (Beta 3/4/5/6, reverse order).
$wt='F:\KK-native-renderer\wt\bench-steady'
$jobs=@('-Mode route -Build beta -Passes 3 -ShotLauncher -Label night1','-Mode route -Build beta -Passes 3 -Label night2')
foreach($e in 2,8,10,1){ foreach($b in 'beta6','beta5','beta4','beta3'){ $jobs+="-Mode chapter -Build $b -Entry $e -Seconds 52 -Scale 3 -Label r2" } }
$log="$wt\kk\out\task147\night.log"
while($jobs.Count) {
 $out=& powershell -NoProfile -ExecutionPolicy Bypass -File "$wt\codex\quiet-147.ps1" -JobList ($jobs -join '|') 2>&1
 $out | Add-Content -LiteralPath $log
 if($out -match 'USER LOCK'){ 'owner is using the PC: stopped' | Add-Content -LiteralPath $log; break }
 $left=@($out | Where-Object {"$_" -like 'REMAINING *'} | ForEach-Object {"$_".Substring(10)})
 if($left.Count -ge $jobs.Count){ Start-Sleep -Seconds 600 }  # no window this time: try again later
 $jobs=$left
 if($jobs.Count){ Start-Sleep -Seconds 120 }  # let others in between windows
}
'night batch done' | Add-Content -LiteralPath $log
