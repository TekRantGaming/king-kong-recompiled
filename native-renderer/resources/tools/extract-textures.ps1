# King Kong texture extractor
# Extracts every texture from your own copy of KKTextures.bf (Peter Jackson's King Kong, Xbox 360)
# and can convert them to DDS and PNG images.

Add-Type -AssemblyName System.Windows.Forms
$ErrorActionPreference = 'Stop'
$bin = Join-Path $PSScriptRoot 'bin'
$bfscan = Join-Path $bin 'kknr_bfscan.exe'
$texdump = Join-Path $bin 'kknr_texdump.exe'

function Ask-YesNo([string]$text, [string]$title) {
    $answer = [System.Windows.Forms.MessageBox]::Show($text, $title, 'YesNo', 'Question')
    return $answer -eq [System.Windows.Forms.DialogResult]::Yes
}

Write-Host ''
Write-Host 'King Kong texture extractor' -ForegroundColor Yellow
Write-Host '---------------------------'

if (-not (Test-Path $bfscan) -or -not (Test-Path $texdump)) {
    Write-Host 'The bin folder is missing. Keep this script next to its bin folder.' -ForegroundColor Red
    Read-Host 'Press Enter to close'
    exit 1
}

# 1. The texture file.
Write-Host ''
Write-Host 'Step 1: choose your KKTextures.bf file (in the game folder).'
$open = New-Object System.Windows.Forms.OpenFileDialog
$open.Title = 'Choose KKTextures.bf'
$open.Filter = 'King Kong texture file (KKTextures.bf)|KKTextures.bf|BF files (*.bf)|*.bf|All files (*.*)|*.*'
if ($open.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Write-Host 'Cancelled.'; exit 0 }
$bf = $open.FileName
Write-Host "  Texture file: $bf"

# 2. The dump location.
Write-Host ''
Write-Host 'Step 2: choose where to put the extracted textures.'
$folder = New-Object System.Windows.Forms.FolderBrowserDialog
$folder.Description = 'Choose a folder for the extracted textures (needs about 25 GB free)'
$folder.ShowNewFolderButton = $true
if ($folder.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Write-Host 'Cancelled.'; exit 0 }
$out = $folder.SelectedPath
$raw = Join-Path $out 'raw'
$images = Join-Path $out 'images'
Write-Host "  Dump folder: $out"

$drive = (Get-Item $out).PSDrive
if ($drive -and $drive.Free -lt 25GB) {
    $freeGB = [math]::Round($drive.Free / 1GB, 1)
    if (-not (Ask-YesNo "Only $freeGB GB is free on that drive. Extracting takes about 4 GB, converting about 17 GB more.`n`nContinue anyway?" 'Low disk space')) { exit 0 }
}

# 3. Extract.
Write-Host ''
Write-Host 'Extracting the textures (a few minutes)...'
New-Item -ItemType Directory -Force -Path $raw | Out-Null
& $bfscan $bf --dump $raw | Tee-Object -FilePath (Join-Path $out 'texture-list.txt') | Select-Object -Last 8
if ($LASTEXITCODE -ne 0) {
    Write-Host 'Extraction failed. Is that the KKTextures.bf from the Xbox 360 version?' -ForegroundColor Red
    Read-Host 'Press Enter to close'
    exit 1
}
$count = (Get-ChildItem $raw -File).Count
Write-Host "  $count textures extracted to $raw" -ForegroundColor Green

# 4. Convert?
if (Ask-YesNo "$count textures were extracted.`n`nConvert them to DDS and PNG images now? (about 17 GB, several minutes)" 'Convert textures') {
    Write-Host ''
    Write-Host 'Converting to DDS and PNG...'
    New-Item -ItemType Directory -Force -Path $images | Out-Null
    $pack = Join-Path $out 'pack-template'
    & $texdump --out $images --csv (Join-Path $out 'textures.csv') --pack-template $pack $raw | Out-File (Join-Path $out 'convert-log.txt') -Encoding utf8
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'Some textures failed to convert; see convert-log.txt.' -ForegroundColor Yellow
    } else {
        Write-Host "  Done: images are in $images" -ForegroundColor Green
        Write-Host "  Texture pack template: $pack (keep pack.txt with the keyed DDS files)." -ForegroundColor Green
    }
    if (Ask-YesNo "Converted. Delete the raw extracted files to save about 4 GB?`n`n(Only needed if you want to convert again.)" 'Clean up') {
        Remove-Item -Recurse -Force $raw
        Write-Host '  Raw files deleted.'
    }
} else {
    Write-Host 'Not converted. Run the script again any time to convert.'
}

Write-Host ''
Write-Host 'Finished.' -ForegroundColor Yellow
Start-Process explorer.exe $out
Read-Host 'Press Enter to close'
