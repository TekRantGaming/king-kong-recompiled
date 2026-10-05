# Builds the README images in docs/images from window screenshots in screenshots/
# (3232 x 1888 window captures; the game area is the 3200 x 1800 inside the frame).
param([string]$Src = "$PSScriptRoot\..\screenshots", [string]$Dst = "$PSScriptRoot\..\docs\images")
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $Dst | Out-Null
$jpeg = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
$params = New-Object System.Drawing.Imaging.EncoderParameters 1
$params.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality, [long]88)
$client = New-Object System.Drawing.Rectangle 16, 72, 3200, 1800

function Save-Scaled($in, $out, $width, [System.Drawing.Rectangle]$crop) {
  $img = [System.Drawing.Image]::FromFile($in)
  $h = [int]($crop.Height * $width / $crop.Width)
  $bmp = New-Object System.Drawing.Bitmap $width, $h
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $g.DrawImage($img, (New-Object System.Drawing.Rectangle 0, 0, $width, $h), $crop, [System.Drawing.GraphicsUnit]::Pixel)
  $bmp.Save($out, $jpeg, $params)
  $g.Dispose(); $bmp.Dispose(); $img.Dispose()
}

$map = [ordered]@{
  'launcher_play' = 'launcher-play'; 'launcher_display' = 'launcher-display'; 'launcher_graphics' = 'launcher-graphics'
  'launcher_gameplay' = 'launcher-gameplay'; 'launcher_controls' = 'launcher-controls'
  'launcher_achievements_list' = 'launcher-achievements'; 'launcher_about' = 'launcher-about'
  'launcher_toast' = 'achievement-popup'
  'shot_menu' = 'game-menu'; 'kk_fix2' = 'game-venture'; 'shot_g09' = 'game-rowboat'; 'shot_w3' = 'game-island'
}
foreach ($k in $map.Keys) { Save-Scaled "$Src\$k.png" "$Dst\$($map[$k]).jpg" 1600 $client }
Get-ChildItem $Dst | Select-Object Name, @{n='KB';e={[int]($_.Length/1KB)}} | Format-Table -AutoSize | Out-String
