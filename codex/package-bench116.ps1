param([string]$Build='F:\KK-native-renderer\wt\bench-rec\out\task115\beta-build')
$ErrorActionPreference='Stop'
$root='F:\KK-native-renderer\testbuilds\beta6-test2-'+(Get-Date -Format MMdd-HHmm)
if (Test-Path -LiteralPath $root) { throw "Package folder already exists: $root" }
$stage="$root\KingKong"
New-Item -ItemType Directory -Path $stage | Out-Null
$base='F:\KK-native-renderer\testbuilds\bench-rec-1015-1617\KingKong'
& robocopy $base $stage /E /XF 'HOW-TO.txt' 'Start Record Benchmark.cmd' /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -gt 7) { throw 'package copy failed' }
$cache=Get-Content -LiteralPath "$Build\CMakeCache.txt" -Raw
if ($cache -notmatch '(?m)^KK_DEV_TOOLS:BOOL=OFF\r?$' -or $cache -notmatch 'sdk-vulkan-upscalers-041/win-amd64') { throw 'Expected player build against permanent SDK 041' }
Copy-Item -LiteralPath "$Build\king_kong.exe","$Build\rexgpu-native.dll","$Build\rexruntime.dll" -Destination $stage
$route='F:\KK-native-renderer\testbuilds\bench-routes\route-45s-1015-1609.kkbr'
Copy-Item -LiteralPath $route -Destination "$stage\benchmark-route.kkbr"
if ((Get-FileHash -LiteralPath "$stage\benchmark-route.kkbr").Hash -ne '4FDAF2B8E8CCDB25D5062DC8AA2734A7197AE9C6FE06043159B256B177527EFD') { throw 'Route hash mismatch' }
$checklist=@'
King Kong Beta 6 test 2: recorded benchmark replay

1. Extract into a separate folder and use your own installed game files.
2. Choose your graphics settings, then About > Run benchmark. Keep the
   bundled benchmark-route.kkbr beside king_kong.exe. Allow three game
   minutes plus loading; do not pause or open F8 during the benchmark.
3. Quit, run it a second time with the same settings, API and frame cap,
   and compare user\benchmarks\run-*\results.txt: chapter average FPS,
   1% low, worst frame and the route-drift warning. Send both result folders
   to Claude. Results also include drift.csv, frames.csv and frame-times.svg.

Known test limitation: automated replays still exceed the position tolerance
and the uncapped chapter averages did not meet the 3% repeatability target.
Kong has no valid recorded position checkpoints. Treat these as diagnostic
results; do not compare them as standardized benchmark scores.

This player build has developer tools disabled and no recording shortcut.
For development, the --kk_benchmark_record=true command-line flag remains.
No game assets are included. The benchmark uses private saves.
'@
$checklist | Set-Content -LiteralPath "$stage\TEST-CHECKLIST.txt" -Encoding ASCII
$checklist | Set-Content -LiteralPath "$root\TEST-CHECKLIST.txt" -Encoding ASCII
$dlls=@('nvngx_dlss.dll','libxess.dll','amd_fidelityfx_dx12.dll','amd_fidelityfx_loader_dx12.dll','amd_fidelityfx_upscaler_dx12.dll','amd_fidelityfx_vk.dll','dxcompiler.dll')
foreach ($name in $dlls) { if (!(Test-Path -LiteralPath "$stage\$name")) { throw "Missing runtime: $name" } }
$strings='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\llvm-strings.exe'
$nativeStrings=& $strings "$stage\rexgpu-native.dll"
foreach ($name in @('NVSDK_NGX','xess')) { if (!($nativeStrings | Select-String -SimpleMatch $name)) { throw "Missing native upscaler symbols: $name" } }
if (Test-Path -LiteralPath "$stage\user\cache\native-shaders") { throw 'Prepared shader cache must not be packaged' }
if (Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object { $_.Name -match '^rexgpu-(xenos|classic)\.' }) { throw 'Classic plugin found' }
$zip="$root\KingKong-beta6-test2-windows-x64.zip"
Push-Location $root
try { & 'C:\Windows\System32\tar.exe' -a -c -f $zip KingKong; if ($LASTEXITCODE) { throw 'tar failed' } }
finally { Pop-Location }
$stream=[IO.File]::OpenRead($zip)
try { if ($stream.ReadByte() -ne 80 -or $stream.ReadByte() -ne 75) { throw 'Zip lacks PK signature' } }
finally { $stream.Dispose() }
$entries=@(& 'C:\Windows\System32\tar.exe' -t -f $zip)
if ($LASTEXITCODE -or $entries -notcontains 'KingKong/benchmark-route.kkbr' -or $entries -notcontains 'KingKong/TEST-CHECKLIST.txt' -or ($entries -match 'Start Record Benchmark|HOW-TO.txt|native-shaders')) { throw 'Unexpected archive contents' }
$hash=(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
"$hash  KingKong-beta6-test2-windows-x64.zip" | Set-Content -LiteralPath "$root\SHA256SUMS.txt" -Encoding ASCII
$files=@(Get-ChildItem -LiteralPath $stage -File -Recurse | ForEach-Object {
 [pscustomobject]@{Path=$_.FullName.Substring($stage.Length+1);Bytes=$_.Length;SHA256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
})
$overlays=@('king_kong.exe','rexgpu-native.dll','rexruntime.dll','TEST-CHECKLIST.txt')
foreach ($file in Get-ChildItem -LiteralPath $base -Recurse -File) {
 $relative=$file.FullName.Substring($base.Length+1)
 if ($relative -in @('HOW-TO.txt','Start Record Benchmark.cmd') -or $relative -in $overlays) { continue }
 if ((Get-FileHash -LiteralPath "$stage\$relative").Hash -ne (Get-FileHash -LiteralPath $file.FullName).Hash) { throw "1617 package content changed: $relative" }
}
$audit=[pscustomobject]@{Package=$root;Zip=$zip;SHA256=$hash;Entries=$entries.Count;DevTools='OFF';RouteSHA256=(Get-FileHash -LiteralPath "$stage\benchmark-route.kkbr").Hash;NativeUpscalerSymbols=@('NVSDK_NGX','xess');Files=$files}
$audit | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$root\build-hashes.json"
$audit | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$PSScriptRoot\..\out\task116\package-audit.json"
$audit | Select-Object Package,Zip,SHA256,Entries,DevTools,RouteSHA256 | Format-List
