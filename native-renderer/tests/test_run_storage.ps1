# Synthetic tests only. No game launches and no files deleted.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'run-storage.ps1')
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$testRoot = Join-Path $repo ('out\task029-storage-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$build = Join-Path $testRoot 'build'
$stage = Join-Path $testRoot 'staging'
New-Item -ItemType Directory -Path $build,"$build\glyphs" -Force | Out-Null
[IO.File]::WriteAllBytes("$build\king_kong.exe", [byte[]](1,2,3,4))
[IO.File]::WriteAllBytes("$build\dxcompiler.dll", [byte[]](5,6,7,8))
[IO.File]::WriteAllBytes("$build\nvngx_dlss.dll", [byte[]](9,10,11,12))
[IO.File]::WriteAllBytes("$build\kkshaders-spirv.pack", [byte[]](13,14,15,16))
Set-Content "$build\glyphs\synthetic.txt" 'private glyph'
Set-Content "$build\mutable.cache" 'must not be staged'
function Check([bool]$Ok, [string]$Message) { if (!$Ok) { throw $Message }; Write-Output "PASS $Message" }
$one = Join-Path $testRoot 'one'; $two = Join-Path $testRoot 'two'
Initialize-RunBuild -Build $build -Run $one -StageRoot $stage
Initialize-RunBuild -Build $build -Run $two -StageRoot $stage
$m1 = Get-Content "$one\build-staging.json" -Raw | ConvertFrom-Json
$m2 = Get-Content "$two\build-staging.json" -Raw | ConvertFrom-Json
Check ($m1.build_hash -eq $m2.build_hash -and $m1.linked_bytes -eq 16 -and $m2.copied_bytes -eq 0) 'identical build reused with hardlinks'
$links = & fsutil hardlink list "$one\dxcompiler.dll"
Check (($links -join ' ') -match '\\two\\dxcompiler.dll') 'two runs and staging reference one NTFS file'
Check (!(Test-Path "$one\mutable.cache")) 'mutable build files excluded'
$blocked = $false
try { [IO.File]::WriteAllBytes("$one\dxcompiler.dll", [byte[]](0)) } catch [UnauthorizedAccessException] { $blocked = $true }
Check $blocked 'shared artifact rejects in-place writes'
Assert-RunBuildUnchanged $one
Set-Content "$one\glyphs\synthetic.txt" 'run writes its glyph copy'
Check ((Get-Content "$two\glyphs\synthetic.txt") -eq 'private glyph') 'glyph copies isolated'
[IO.File]::WriteAllBytes("$build\dxcompiler.dll", [byte[]](17,18,19,20))
Assert-RunBuildUnchanged $one
$three = Join-Path $testRoot 'three'
Initialize-RunBuild -Build $build -Run $three -StageRoot $stage
$m3 = Get-Content "$three\build-staging.json" -Raw | ConvertFrom-Json
Check ($m1.build_hash -ne $m3.build_hash) 'changed build produces a new stage'
# Force the same failure a different volume or unsupported filesystem returns.
function New-RunArtifactLink([string]$Path, [string]$Target) { throw [IO.IOException]::new('synthetic link failure') }
$four = Join-Path $testRoot 'four'
Initialize-RunBuild -Build $build -Run $four -StageRoot $stage
$m4 = Get-Content "$four\build-staging.json" -Raw | ConvertFrom-Json
Check ($m4.linked_bytes -eq 0 -and $m4.copied_bytes -eq 16) 'link failure falls back to private copies'
Assert-RunBuildUnchanged $four
$refused = $false
try { Initialize-RunBuild -Build $build -Run $one -StageRoot $stage } catch { $refused = $true }
Check $refused 'existing run artifacts never overwritten'
Write-Output "Synthetic evidence retained in $testRoot"
