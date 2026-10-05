<#
.SYNOPSIS
  One-time setup: downloads the ReXGlue SDK, extracts your King Kong disc
  image, pulls the achievement names and icons out of your default.xex, and
  generates the recompiled C++ sources.

.EXAMPLE
  .\setup.ps1 -Iso "D:\Xbox 360\King Kong.iso"
  then: kk\build.bat
#>
param(
    # Path to your own Peter Jackson's King Kong Xbox 360 disc image (.iso).
    [Parameter(Mandatory = $true)][string]$Iso
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$sdkVersion = '0.10.0'
$sdkDir = Join-Path $root 'tools\rexglue'
$rexglue = Join-Path $sdkDir 'win-amd64\bin\rexglue.exe'
$assets = Join-Path $root 'kk\assets'

# 1. ReXGlue SDK (prebuilt release)
if (-not (Test-Path $rexglue)) {
    $zip = Join-Path $env:TEMP "rexglue-sdk-$sdkVersion-win-amd64.zip"
    $url = "https://github.com/rexglue/rexglue-sdk/releases/download/v$sdkVersion/rexglue-sdk-$sdkVersion-win-amd64.zip"
    Write-Host "Downloading ReXGlue SDK $sdkVersion..."
    Invoke-WebRequest -Uri $url -OutFile $zip
    Expand-Archive -Path $zip -DestinationPath $sdkDir -Force
}

# 2. Extract the disc into kk/assets (skipped if already there)
if (-not (Test-Path (Join-Path $assets 'KKTextures.bf'))) {
    Write-Host "Extracting $Iso -> $assets (about 6.3 GB)"
    & (Join-Path $root 'tools\extract_iso.ps1') -Iso $Iso -OutDir $assets
}
if (-not (Test-Path (Join-Path $assets 'default.xex'))) { throw "default.xex not found after extraction" }

# rexglue logs to stderr; in Windows PowerShell 5.1 that would trip 'Stop', so judge by exit code.
$ErrorActionPreference = 'Continue'
Push-Location (Join-Path $root 'kk')
try {
    # 3. Achievement names and icons, read from your default.xex
    & $rexglue init --project-name king_kong --xex-path assets/default.xex achievements `
        (Join-Path $assets 'default.xex') (Join-Path $assets 'achievements') 2>&1 | ForEach-Object { "$_" }

    # 4. Generate recompiled sources
    Write-Host "Running rexglue codegen..."
    & $rexglue codegen king_kong_manifest.toml 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "codegen failed ($LASTEXITCODE)" }
} finally { Pop-Location; $ErrorActionPreference = 'Stop' }

Write-Host "`nDone. Build with: kk\build.bat   Run with: kk\run.bat"
