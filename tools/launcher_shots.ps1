# README launcher screenshots. Starts the developer build with KK_DEV_LAUNCHER_TOUR so the
# launcher shows each page in turn, and saves each one when the log says it is up.
# Only the launcher window's own content is captured (PrintWindow), never other windows on
# screen. Writes screenshots\launcher_<page>.png and docs\images\launcher-<page>.jpg.
param(
  [string]$Exe = "$PSScriptRoot\..\kk\out\build\kk-dev\king_kong.exe",
  [int]$Width = 3200, [int]$Height = 1800, [double]$Each = 3.0, [string[]]$Extra = @()
)
$root = (Resolve-Path "$PSScriptRoot\..").Path
$shots = "$root\screenshots"; $docs = "$root\docs\images"
New-Item -ItemType Directory -Force $shots, $docs | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class KkWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[KkWin]::SetProcessDPIAware() | Out-Null
$jpeg = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
$q = New-Object System.Drawing.Imaging.EncoderParameters 1
$q.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality, [long]88)

function Save-Window($hwnd, $name) {
  $r = New-Object KkWin+RECT
  [KkWin]::GetClientRect($hwnd, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $dc = $g.GetHdc()
  [KkWin]::PrintWindow($hwnd, $dc, 3) | Out-Null  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
  $g.ReleaseHdc($dc); $g.Dispose()
  $bmp.Save("$shots\launcher_$name.png", [System.Drawing.Imaging.ImageFormat]::Png)
  $h = [int]($bmp.Height * 1600 / $bmp.Width)
  $small = New-Object System.Drawing.Bitmap 1600, $h
  $g2 = [System.Drawing.Graphics]::FromImage($small)
  $g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $g2.DrawImage($bmp, 0, 0, 1600, $h)
  $small.Save("$docs\launcher-$($name -replace '_', '-').jpg", $jpeg, $q)
  $g2.Dispose(); $small.Dispose(); $bmp.Dispose()
  "saved $name"
}

$dir = Split-Path $Exe
$log = "launcher_tour_$(Get-Date -Format HHmmss).log"
$env:KK_DEV_LAUNCHER_TOUR = "$Each"
$argList = @("--log_file=$log", "--window_width=$Width", "--window_height=$Height", "--fullscreen=false",
             "--kk_check_updates=false") + $Extra
$p = Start-Process $Exe -WorkingDirectory $dir -PassThru -ArgumentList $argList
$env:KK_DEV_LAUNCHER_TOUR = ""
$done = @{}
for ($i = 0; $i -lt 600 -and -not $p.HasExited; $i++) {
  Start-Sleep -Milliseconds 250
  $lines = Get-Content "$dir\$log" -ErrorAction SilentlyContinue | Select-String "KK dev: tour (\w+)"
  foreach ($m in $lines) {
    $name = $m.Matches[0].Groups[1].Value
    if ($done.ContainsKey($name)) { continue }
    $done[$name] = $true
    if ($name -eq "done") { break }
    Start-Sleep -Milliseconds ([int]($Each * 450))  # let the page settle
    $p.Refresh()
    Save-Window $p.MainWindowHandle $name
  }
  if ($done.ContainsKey("done")) { break }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
