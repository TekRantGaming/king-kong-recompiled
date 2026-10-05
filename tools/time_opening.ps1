# Dev helper: times the scripted opening (Play -> Intro video -> VENTURE -> rowboat -> white flash on
# reaching the island) at a given frame-rate cap, by sampling the window's brightness every 2 s.
param([int]$FrameRate = 120, [int]$Seconds = 240, [string]$Tag = "fps")
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$d = "$root\kk\out\build\kk-relwithdebinfo"; $t = $PSScriptRoot
$p = Start-Process "$d\king_kong.exe" -WorkingDirectory $d -PassThru -ArgumentList "--game_data_root=`"$($root.Replace('\','/'))/kk/assets`"", "--log_file=time_$Tag.log", '--kk_launcher=false', '--mnk_mode=true', "--kk_frame_rate=$FrameRate"
$id = $p.Id; Start-Sleep 20
for ($i = 0; $i -lt 8; $i++) { & "$t\send_keys.ps1" -ProcessId $id -Keys Return; Start-Sleep 3 }
& "$t\send_keys.ps1" -ProcessId $id -Keys Space; Start-Sleep 5   # pick the save
& "$t\send_keys.ps1" -ProcessId $id -Keys Space; Start-Sleep 4   # load OK
& "$t\send_keys.ps1" -ProcessId $id -Keys Space                  # Play
$t0 = Get-Date
$tmp = "$env:TEMP\kk_time.png"
$out = @()
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
  & "$t\snap_window.ps1" -ProcessId $id -OutFile $tmp | Out-Null
  $img = [System.Drawing.Bitmap]::FromFile($tmp); $sum = 0; $n = 0
  for ($y = 120; $y -lt $img.Height - 40; $y += 60) { for ($x = 40; $x -lt $img.Width - 40; $x += 60) { $c = $img.GetPixel($x, $y); $sum += ($c.R + $c.G + $c.B) / 3; $n++ } }
  $img.Dispose()
  $out += "{0,6:N1} {1,5:N0}" -f ((Get-Date) - $t0).TotalSeconds, ($sum / $n)
  Start-Sleep -Milliseconds 1200
}
$p.CloseMainWindow() | Out-Null
$out
