param([int[]]$Reps=@(1,2), [string[]]$Builds=@('beta3','beta4','beta5','beta6'), [int[]]$Entries=@(2,8,10,1))
# Task 147 Part 2: every build idles at the start of each benchmark chapter (no input) for the
# same window. One launch per call of run-147.ps1, so the game lock is free between launches.
$ErrorActionPreference='Continue'
$runner='F:\KK-native-renderer\wt\bench-steady\codex\run-147.ps1'
foreach($rep in $Reps) {
 $order=if($rep % 2){$Builds}else{[array]::Reverse($Builds); $Builds}
 foreach($entry in $Entries) {
  foreach($build in $order) {
   Write-Output "=== rep $rep entry $entry $build $(Get-Date -Format HH:mm:ss)"
   & powershell -NoProfile -ExecutionPolicy Bypass -File $runner -Mode chapter -Build $build -Entry $entry -Seconds 52 -Label "r$rep" 2>&1 |
    Select-String -Pattern 'COMPLETE|LAUNCHED|incomplete|USER LOCK|Exception|error' | ForEach-Object { $_.Line }
  }
 }
}
