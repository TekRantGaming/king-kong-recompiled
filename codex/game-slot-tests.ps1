param([Parameter(Mandatory)][string]$OutputRoot)
$ErrorActionPreference='Stop'
$helper=Join-Path $PSScriptRoot 'game-slot.ps1'
. $helper
$root=Join-Path $OutputRoot ('game-slot-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null
$root=(Resolve-Path -LiteralPath $root).Path
$lock=Join-Path $root 'slot.lock'
$priority=Join-Path $root 'priority'
$emptyGames={ @() }
function Check([bool]$condition,[string]$message) { if (!$condition) { throw $message } }

# Private files only: these tests never claim the real slot or start a game.
Set-Content -LiteralPath $lock -Value 'user watching video' -Encoding ascii
$before=[IO.File]::ReadAllBytes($lock)
$claim=Try-ClaimGameSlot -Task task100 -LockPath $lock -PriorityPath $priority -GetGames $emptyGames
Check ($claim.State -eq 'user' -and !$claim.Guard) 'User reservation was not respected.'
Check ([Convert]::ToBase64String($before) -eq [Convert]::ToBase64String([IO.File]::ReadAllBytes($lock))) 'User lock was modified.'
Set-Content -LiteralPath $lock -Value free -Encoding ascii
Set-Content -LiteralPath $priority -Value task111 -Encoding ascii
$claim=Try-ClaimGameSlot -Task task098 -LockPath $lock -PriorityPath $priority -GetGames $emptyGames
Check (!$claim.Guard -and (Read-GameSlotState $lock) -eq 'free') 'Competing priority was ignored.'
Set-Content -LiteralPath $priority -Value task098 -Encoding ascii
$claim=Try-ClaimGameSlot -Task task098 -LockPath $lock -PriorityPath $priority -GetGames {
 @([pscustomobject]@{ExecutablePath='C:\Users\Player\Desktop\KingKong\king_kong.exe'})
}
Check (!$claim.Guard -and $claim.UserGame) 'User game must block a claim.'
$claim=Try-ClaimGameSlot -Task task098 -LockPath $lock -PriorityPath $priority -GetGames $emptyGames
Check ([bool]$claim.Guard) 'Free slot was not claimed.'
try {
 $other=Try-ClaimGameSlot -Task task098 -LockPath $lock -PriorityPath $priority -GetGames $emptyGames
 Check (!$other.Guard) 'A second writer acquired a held slot.'
 $overwrote=$false
 try { Set-Content -LiteralPath $lock -Value user -Encoding ascii -ErrorAction Stop; $overwrote=$true } catch [IO.IOException] {}
 Check (!$overwrote) 'A legacy Set-Content writer overwrote a held slot.'
} finally { Release-GameSlot $claim.Guard }
Check ((Read-GameSlotState $lock) -eq 'free') 'Release did not free the slot.'

# Race two independent PowerShell processes, including atomic initialization of
# a missing file. Both stay alive until the parent has checked the outcome.
foreach ($round in 1..2) {
 $race=Join-Path $root "race-$round.lock"
 $gate=Join-Path $root "start-$round"
 $release=Join-Path $root "release-$round"
 $jobs=@()
 try {
  foreach ($id in 1..2) {
   $jobs+=Start-Job -ArgumentList $helper,$race,$priority,$gate,$release,$root,$round,$id -ScriptBlock {
    param($helper,$race,$priority,$gate,$release,$root,$round,$id)
    $ErrorActionPreference='Stop'
    . $helper
    Set-Content -LiteralPath "$root/ready-$round-$id" -Value ready
    while (!(Test-Path -LiteralPath $gate)) { Start-Sleep -Milliseconds 10 }
    $claim=Try-ClaimGameSlot -Task task098 -LockPath $race -PriorityPath $priority -GetGames { @() }
    try {
     Set-Content -LiteralPath "$root/result-$round-$id" -Value ([bool]$claim.Guard).ToString()
     $deadline=[DateTime]::UtcNow.AddSeconds(20)
     while (!(Test-Path -LiteralPath $release)) {
      if ([DateTime]::UtcNow -gt $deadline) { throw 'Parent did not release race test.' }
      Start-Sleep -Milliseconds 10
     }
    } finally { Release-GameSlot $claim.Guard }
   }
  }
  $deadline=[DateTime]::UtcNow.AddSeconds(20)
  foreach ($prefix in 'ready','result') {
   while (!(Test-Path -LiteralPath "$root/$prefix-$round-1") -or !(Test-Path -LiteralPath "$root/$prefix-$round-2")) {
    if ([DateTime]::UtcNow -gt $deadline) { throw "Race test timed out: $prefix" }
    Start-Sleep -Milliseconds 10
   }
   if ($prefix -eq 'ready') { Set-Content -LiteralPath $gate -Value start }
  }
  $wins=@(1..2 | Where-Object { (Get-Content -LiteralPath "$root/result-$round-$_" -Raw).Trim() -eq 'True' }).Count
  Check ($wins -eq 1) "Race admitted $wins owners."
 } finally {
  Set-Content -LiteralPath $release -Value release
  $jobs | Wait-Job -Timeout 25 | Out-Null
  $jobs | Receive-Job -ErrorAction Stop
  Check (@($jobs | Where-Object State -ne Completed).Count -eq 0) 'A race worker failed.'
 }
 Check ((Read-GameSlotState $race) -eq 'free') 'Race winner did not release the slot.'
}
Write-Output 'Atomic shared game-slot claims, user/priority protection and concurrent initialization: PASS'
