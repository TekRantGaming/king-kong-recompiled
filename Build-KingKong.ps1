<#
  King Kong builder.

  Builds the PC port on this computer from your own copy of the Xbox 360
  game disc. Nothing from the game is downloaded or included: the game code
  is translated and compiled here, from your file.

  Steps: check build tools (offer to install them), pick your disc image,
  download the ReXGlue SDK, translate the game code, compile, and put the
  finished game in the KingKong folder.
#>
param(
    [string]$Iso,                         # skip the file picker
    [string]$OutDir = "$PSScriptRoot\KingKong",
    [switch]$Yes,                         # answer yes to every question
    [switch]$NoShortcut,                  # never add a desktop shortcut
    [switch]$NoLaunch                     # do not offer to start the game
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$titleId = 0x555307D3

function Say($text, $color = 'Gray') { Write-Host $text -ForegroundColor $color }
function Step($n, $text) { Write-Host ""; Write-Host " $n  $text" -ForegroundColor Green }
function Ask($question) {
    if ($Yes) { return $true }
    $a = Read-Host "$question [Y/n]"
    return ($a -eq '' -or $a -match '^[Yy]')
}
function Fail($text) { Write-Host ""; Write-Host " $text" -ForegroundColor Red; Read-Host "Press Enter to close"; exit 1 }

Clear-Host
Say ""
Say "  PETER JACKSON.S KING KONG  -  PC port builder" 'Green'
Say "  Builds the game on this PC from your own disc image."
Say "  This takes about 20 to 40 minutes and needs about 15 GB free."

# ---------------------------------------------------------------- 1. tools ---
Step 1 "Checking build tools"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
function Find-VS { if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath } }
function Have($exe, $fallback) { (Get-Command $exe -ErrorAction SilentlyContinue) -or ($fallback -and (Test-Path $fallback)) }

$missing = @()
if (-not (Find-VS)) { $missing += 'vs' }
if (-not (Have cmake "$env:ProgramFiles\CMake\bin\cmake.exe")) { $missing += 'cmake' }
if (-not (Have ninja "")) { $missing += 'ninja' }

if ($missing.Count) {
    Say "  Missing:" 'Yellow'
    if ($missing -contains 'vs') { Say "   - Visual Studio 2022 Build Tools with C++ and Clang (about 6 GB)" 'Yellow' }
    if ($missing -contains 'cmake') { Say "   - CMake" 'Yellow' }
    if ($missing -contains 'ninja') { Say "   - Ninja" 'Yellow' }
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { Fail "winget is not available. Install the tools above yourself, then run this again." }
    if (-not (Ask "  Install them now with winget? Windows will ask for permission")) { Fail "The build tools are needed. Install them, then run this again." }
    if ($missing -contains 'cmake') { winget install --id Kitware.CMake -e --accept-package-agreements --accept-source-agreements --silent }
    if ($missing -contains 'ninja') { winget install --id Ninja-build.Ninja -e --accept-package-agreements --accept-source-agreements --silent }
    if ($missing -contains 'vs') {
        Say "  Installing Visual Studio Build Tools. This can take a while." 'Yellow'
        winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-package-agreements --accept-source-agreements --override "--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset --add Microsoft.VisualStudio.Component.Windows11SDK.26100"
    }
    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
    if (-not (Find-VS)) { Fail "Visual Studio Build Tools still not found. Restart your PC and run this again." }
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { $env:Path += ";$env:ProgramFiles\CMake\bin" }
Say "  Build tools ready." 'Green'

# ----------------------------------------------------------------- 2. disc ---
Step 2 "Choosing your King Kong disc image"
if (-not $Iso) {
    Say "  Pick your Peter Jackson's King Kong Xbox 360 disc image (.iso)."
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = "Select your King Kong Xbox 360 disc image"
    $dlg.Filter = 'Xbox 360 disc image (*.iso)|*.iso|All files|*.*'
    if ($dlg.ShowDialog() -ne 'OK') { Fail "No disc image chosen." }
    $Iso = $dlg.FileName
}
if (-not (Test-Path -LiteralPath $Iso)) { Fail "File not found: $Iso" }
Add-Type -Path "$root\tools\XisoExtract.cs"
$id = [XisoExtract]::TitleId((Resolve-Path -LiteralPath $Iso).Path)
if ($id -eq 0) { Fail "That file is not an Xbox 360 disc image." }
if ($id -ne $titleId) { Fail ("That disc is title {0:X8}, not King Kong ({1:X8})." -f $id, $titleId) }
$drive = (Get-Item -LiteralPath $root).PSDrive
if ($drive.Free -lt 15GB) { Fail ("About 15 GB of free space is needed on {0}: (found {1:N0} GB)." -f $drive.Name, ($drive.Free / 1GB)) }
Say "  Found King Kong ($([IO.Path]::GetFileName($Iso)))." 'Green'

# ------------------------------------------------- 3. extract and translate ---
Step 3 "Unpacking the disc and translating the game code"
& "$root\setup.ps1" -Iso $Iso
if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) { Fail "Setup failed (see above)." }

# -------------------------------------------------------------- 4. compile ---
Step 4 "Compiling (the long part)"
cmd /c "`"$root\kk\build.bat`" kk-release"
if ($LASTEXITCODE -ne 0) { Fail "Compiling failed (see above)." }

# ---------------------------------------------------------------- 5. stage ---
Step 5 "Putting the game together"
$bin = "$root\kk\out\build\kk-release"
New-Item -ItemType Directory -Force $OutDir | Out-Null
Copy-Item "$bin\*.exe", "$bin\*.dll" $OutDir -Force
# Move (not copy) the 6.3 GB of game files so they aren't stored twice.
if (Test-Path "$OutDir\game") { Move-Item "$OutDir\game" "$OutDir\game.old" -Force }
Move-Item "$root\kk\assets" "$OutDir\game"
New-Item -ItemType Directory -Force "$root\kk\assets" | Out-Null
$exe = "$OutDir\king_kong.exe"
Say "  Done: $exe" 'Green'

if (-not $NoShortcut -and (Ask "  Add a desktop shortcut?")) {
    $lnk = Join-Path ([Environment]::GetFolderPath('Desktop')) 'King Kong.lnk'
    $sh = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk)
    $sh.TargetPath = $exe; $sh.WorkingDirectory = $OutDir; $sh.Save()
    Say "  Shortcut added." 'Green'
}
Say ""
Say "  All done. Hold Shift while starting the game to open the launcher at any time." 'Green'
if (-not $NoLaunch -and (Ask "  Start King Kong now?")) { Start-Process $exe -WorkingDirectory $OutDir }
