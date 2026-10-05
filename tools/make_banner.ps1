# Composes docs/images/banner.jpg: the game's main menu (logo, moon and cliffs
# above the menu text) with the port's tagline.
param(
  [string]$Menu = "$PSScriptRoot\..\screenshots\shot_menu.png",
  [string]$Out = "$PSScriptRoot\..\docs\images\banner.jpg",
  # Band of the menu screenshot to use (window capture at 3232 x 1888).
  [int]$X = 23, [int]$Y = 162, [int]$W = 3201, [int]$H = 600
)
Add-Type -AssemblyName System.Drawing
$OW = 1920; $OH = [int]($H * $OW / $W)
$bmp = New-Object System.Drawing.Bitmap $OW, $OH
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

$src = [System.Drawing.Image]::FromFile($Menu)
$g.DrawImage($src, (New-Object System.Drawing.Rectangle 0, 0, $OW, $OH), (New-Object System.Drawing.Rectangle $X, $Y, $W, $H), [System.Drawing.GraphicsUnit]::Pixel)
$src.Dispose()

# Darken the right side for the tagline.
$shade = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.Point ([int]($OW * 0.5) - 2), 0), (New-Object System.Drawing.Point ($OW + 2), 0), ([System.Drawing.Color]::FromArgb(0, 9, 8, 7)), ([System.Drawing.Color]::FromArgb(190, 9, 8, 7))
$shade.WrapMode = [System.Drawing.Drawing2D.WrapMode]::TileFlipX
$g.FillRectangle($shade, [int]($OW * 0.5), 0, $OW - [int]($OW * 0.5), $OH)

$big = New-Object System.Drawing.Font "Segoe UI", 56, ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
$small = New-Object System.Drawing.Font "Segoe UI Semibold", 30, ([System.Drawing.FontStyle]::Regular), ([System.Drawing.GraphicsUnit]::Pixel)
$shadow = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(170, 0, 0, 0))
$white = [System.Drawing.Brushes]::White
$amber = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 232, 163, 60))
$right = New-Object System.Drawing.StringFormat; $right.Alignment = [System.Drawing.StringAlignment]::Far
$tx = $OW - 70; $ty = [int]($OH * 0.36)
$g.DrawString("Native PC port", $big, $shadow, $tx + 3, $ty + 3, $right)
$g.DrawString("Native PC port", $big, $white, $tx, $ty, $right)
$g.DrawString("of the Xbox 360 version", $small, $amber, $tx, $ty + 78, $right)

$jpeg = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
$p = New-Object System.Drawing.Imaging.EncoderParameters 1
$p.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality, [long]90)
$bmp.Save($Out, $jpeg, $p)
$g.Dispose(); $bmp.Dispose()
"banner: $Out ($OW x $OH)"
