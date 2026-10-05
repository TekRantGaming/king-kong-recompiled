# Dev helper: start the game, skip the intros, load the first save and skip the story videos so
# the opening on the ship plays, then wait $Seconds and close. Prints the process id.
param([int]$FrameRate = 120, [int]$Seconds = 90, [string]$Log = "ship.log")
$root = Split-Path $PSScriptRoot -Parent
$d = "$root\kk\out\build\kk-relwithdebinfo"; $t = $PSScriptRoot
$p = Start-Process "$d\king_kong.exe" -WorkingDirectory $d -PassThru -ArgumentList "--game_data_root=`"$($root.Replace('\','/'))/kk/assets`"", "--log_file=$Log", '--kk_launcher=false', '--mnk_mode=true', "--kk_frame_rate=$FrameRate"
$id = $p.Id; Start-Sleep 20
for ($i = 0; $i -lt 8; $i++) { & "$t\send_keys.ps1" -ProcessId $id -Keys Return; Start-Sleep 3 }
& "$t\send_keys.ps1" -ProcessId $id -Keys Space; Start-Sleep 5   # pick the save
& "$t\send_keys.ps1" -ProcessId $id -Keys Space; Start-Sleep 4   # load OK
& "$t\send_keys.ps1" -ProcessId $id -Keys Space; Start-Sleep 8   # Play
for ($i = 0; $i -lt 6; $i++) { & "$t\send_keys.ps1" -ProcessId $id -Keys Return; Start-Sleep 5 }
$shipAt = [int]((Get-Date) - $p.StartTime).TotalSeconds
Start-Sleep $Seconds
$p.CloseMainWindow() | Out-Null; Start-Sleep 3
"pid=$id ship_at=${shipAt}s"
