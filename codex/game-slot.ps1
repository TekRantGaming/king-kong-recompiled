# Shared atomic claim/release contract. Keep the returned stream open until all
# owned game processes have exited. Readers may inspect it; other writers cannot.
function Read-GameSlotState {
 param([string]$LockPath='F:\KK-native-renderer\runs\codex-game.lock')
 try { return (Get-Content -LiteralPath $LockPath -Raw -ErrorAction Stop).Trim() }
 catch { return 'busy' }
}

function Try-ClaimGameSlot {
 param([Parameter(Mandatory)][string]$Task,
       [string]$LockPath='F:\KK-native-renderer\runs\codex-game.lock',
       [string]$PriorityPath='F:\KK-native-renderer\runs\codex-game.priority',
       [scriptblock]$GetGames={ @(Get-CimInstance Win32_Process -Filter "Name='king_kong.exe'") },
       [switch]$RequireBackgroundHelper)
 $stream=$null
 $claimed=$false
 try {
  try {
   # CreateNew initializes only a file we created, never an existing user lock.
   $stream=[IO.File]::Open($LockPath,'CreateNew','ReadWrite','Read')
   $bytes=[Text.Encoding]::ASCII.GetBytes("free`r`n")
   $stream.Write($bytes,0,$bytes.Length); $stream.Flush()
  } catch [IO.IOException] {
   try { $stream=[IO.File]::Open($LockPath,'Open','ReadWrite','Read') }
   catch [IO.IOException] { return [pscustomobject]@{State='busy';Guard=$null;UserGame=$false} }
  }
  $stream.Position=0
  $reader=[IO.StreamReader]::new($stream,[Text.Encoding]::UTF8,$true,1024,$true)
  try { $state=$reader.ReadToEnd().Trim() } finally { $reader.Dispose() }
  if ($state.StartsWith('user')) { return [pscustomobject]@{State='user';Guard=$null;UserGame=$false} }
  if ($state -ne 'free') { return [pscustomobject]@{State=$state;Guard=$null;UserGame=$false} }
  $priority=if (Test-Path -LiteralPath $PriorityPath) { (Get-Content -LiteralPath $PriorityPath -Raw -ErrorAction Stop).Trim() } else { '' }
  if ($priority -and $priority -notmatch ('^'+[regex]::Escape($Task)+'(?:\s|$)')) {
   return [pscustomobject]@{State="priority $priority";Guard=$null;UserGame=$false}
  }
  $games=@(& $GetGames)
  if ($games.Count) {
   $userGame=@($games | Where-Object { !$_.ExecutablePath -or
    ($_.ExecutablePath -notlike 'F:\KK-native-renderer\runs\*' -and $_.ExecutablePath -notlike 'F:\KK-native-renderer\wt\*') }).Count -gt 0
   return [pscustomobject]@{State='game running';Guard=$null;UserGame=$userGame}
  }
  if ($RequireBackgroundHelper -and !(Get-Process kkbg2 -ErrorAction SilentlyContinue)) {
   throw 'kkbg2 must already be running.'
  }
  $token="$Task $(Get-Date -Format o)"
  $bytes=[Text.Encoding]::ASCII.GetBytes("$token`r`n")
  $stream.SetLength(0); $stream.Position=0
  $stream.Write($bytes,0,$bytes.Length); $stream.Flush()
  $claimed=$true
  return [pscustomobject]@{State='claimed';UserGame=$false;Guard=[pscustomobject]@{Stream=$stream;Token=$token}}
 } finally { if ($stream -and !$claimed) { $stream.Dispose() } }
}

function Wait-GameSlot {
 param([Parameter(Mandatory)][string]$Task,
       [string]$LockPath='F:\KK-native-renderer\runs\codex-game.lock',
       [string]$PriorityPath='F:\KK-native-renderer\runs\codex-game.priority',
       [switch]$RequireBackgroundHelper)
 while ($true) {
  $claim=Try-ClaimGameSlot -Task $Task -LockPath $LockPath -PriorityPath $PriorityPath -RequireBackgroundHelper:$RequireBackgroundHelper
  if ($claim.State -eq 'user') { throw 'USER LOCK: code-only work; no game launch.' }
  if ($claim.Guard) { return $claim.Guard }
  Write-Output "Waiting for game slot: $($claim.State)" | Out-Host
  Start-Sleep -Seconds $(if ($claim.UserGame) {60} else {30})
 }
}

function Release-GameSlot {
 param($Guard)
 if (!$Guard) { return }
 try {
  $Guard.Stream.Position=0
  $reader=[IO.StreamReader]::new($Guard.Stream,[Text.Encoding]::UTF8,$true,1024,$true)
  try { $state=$reader.ReadToEnd().Trim() } finally { $reader.Dispose() }
  if ($state -eq $Guard.Token -and !$state.StartsWith('user')) {
   $bytes=[Text.Encoding]::ASCII.GetBytes("free`r`n")
   $Guard.Stream.SetLength(0); $Guard.Stream.Position=0
   $Guard.Stream.Write($bytes,0,$bytes.Length); $Guard.Stream.Flush()
  }
 } finally { $Guard.Stream.Dispose() }
}
