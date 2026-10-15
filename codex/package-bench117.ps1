param([string]$Build='F:\KK-native-renderer\wt\bench-rec\out\task115\beta-build')
$ErrorActionPreference='Stop'
$root='F:\KK-native-renderer\testbuilds\beta6-test3-'+(Get-Date -Format MMdd-HHmm)
if (Test-Path -LiteralPath $root) { throw "Package folder already exists: $root" }
$stage="$root\KingKong"
New-Item -ItemType Directory -Path $stage | Out-Null
$base='F:\KK-native-renderer\testbuilds\beta6-test2-1015-1649\KingKong'
& robocopy $base $stage /E /XF 'HOW-TO.txt' 'Start Record Benchmark.cmd' /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -gt 7) { throw 'package copy failed' }
$cache=Get-Content -LiteralPath "$Build\CMakeCache.txt" -Raw
if ($cache -notmatch '(?m)^KK_DEV_TOOLS:BOOL=OFF\r?$' -or $cache -notmatch 'sdk-vulkan-upscalers-041/win-amd64') { throw 'Expected player build against permanent SDK 041' }
Copy-Item -LiteralPath "$Build\king_kong.exe","$Build\rexgpu-native.dll","$Build\rexruntime.dll" -Destination $stage
$route='F:\KK-native-renderer\testbuilds\bench-routes\route-45s-1015-1609.kkbr'
Copy-Item -LiteralPath $route -Destination "$stage\benchmark-route.kkbr"
if ((Get-FileHash -LiteralPath "$stage\benchmark-route.kkbr").Hash -ne '4FDAF2B8E8CCDB25D5062DC8AA2734A7197AE9C6FE06043159B256B177527EFD') { throw 'Route hash mismatch' }
$checklist=@'
King Kong Beta 6 test 3: benchmark UI and live monitor

1. Extract into a separate folder and use your installed game files.
2. Choose graphics settings, then About > Run benchmark > Start benchmark.
   Keep benchmark-route.kkbr beside king_kong.exe. Allow three game minutes
   plus loading. Do not pause or open F8 during the benchmark.
3. Check the live monitor, then the Total and four Chapter result tabs.
   Scroll to view all counters, the system information and measurement notes.
   Check English, German, Spanish, French and Italian at your normal UI scale.
4. Save results rewrites the local results.txt and summary.csv. Copy results
   copies text; Copy CSV copies the aggregate table. Nothing is uploaded.
5. Quit and run again with the same settings, API and frame cap. The newest
   previous run in this user data root is shown beside the new measurements.
   Send both result folders under userenchmarks to Claude. Each contains
   results.txt, summary.csv, frames.csv, drift.csv and frame-times.svg.
6. Owner checks still needed: real mouse/controller start/save/copy actions,
   the secondary DirectX 12 API, other GPUs and missing-counter behavior.

Latency means age of the latest observed game tick at guest swap. Title
threads are not paired by frame ID. It excludes Present, display and input
latency. GPU % is the busiest process
GPU Engine counter. CPU % uses all logical processors. GPU frame timing covers
the scene command list, excluding the presenter/UI. Thread times are execution
CPU time per rendered frame. VRAM uses the active API's memory-budget query.
Counter means are time-weighted; peaks are sampled peaks. Negative CSV values
mean unavailable. Vendor clock/temperature/power rows appear only if the
installed vendor API provides them. No additional runtime is installed.

Known limitations: the supplied route still reports drift and Kong lacks valid
position checkpoints. These are diagnostic results, not standardized scores.
A single monitor-on/off pair cannot establish a small performance difference
on this variable route. See REPORT-117 for the measured cost and test limits.

Developer tools are disabled. All three upscalers are retained; there are no
game assets, Classic renderer or prepared native-shader cache. Private saves
are used. The inherited executable version string is 2.0.0-beta.5.
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
$zip="$root\KingKong-beta6-test3-windows-x64.zip"
Push-Location $root
try { & 'C:\Windows\System32\tar.exe' -a -c -f $zip KingKong; if ($LASTEXITCODE) { throw 'tar failed' } }
finally { Pop-Location }
$stream=[IO.File]::OpenRead($zip)
try { if ($stream.ReadByte() -ne 80 -or $stream.ReadByte() -ne 75) { throw 'Zip lacks PK signature' } }
finally { $stream.Dispose() }
$entries=@(& 'C:\Windows\System32\tar.exe' -t -f $zip)
if ($LASTEXITCODE -or $entries -notcontains 'KingKong/benchmark-route.kkbr' -or $entries -notcontains 'KingKong/TEST-CHECKLIST.txt' -or ($entries -match 'Start Record Benchmark|HOW-TO.txt|native-shaders')) { throw 'Unexpected archive contents' }
$hash=(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
"$hash  KingKong-beta6-test3-windows-x64.zip" | Set-Content -LiteralPath "$root\SHA256SUMS.txt" -Encoding ASCII
$files=@(Get-ChildItem -LiteralPath $stage -File -Recurse | ForEach-Object {
 [pscustomobject]@{Path=$_.FullName.Substring($stage.Length+1);Bytes=$_.Length;SHA256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
})
$overlays=@('king_kong.exe','rexgpu-native.dll','rexruntime.dll','TEST-CHECKLIST.txt')
foreach ($file in Get-ChildItem -LiteralPath $base -Recurse -File) {
 $relative=$file.FullName.Substring($base.Length+1)
 if ($relative -in @('HOW-TO.txt','Start Record Benchmark.cmd') -or $relative -in $overlays) { continue }
 if ((Get-FileHash -LiteralPath "$stage\$relative").Hash -ne (Get-FileHash -LiteralPath $file.FullName).Hash) { throw "test 2 package content changed: $relative" }
}
$audit=[pscustomobject]@{Package=$root;Zip=$zip;SHA256=$hash;Entries=$entries.Count;DevTools='OFF';RouteSHA256=(Get-FileHash -LiteralPath "$stage\benchmark-route.kkbr").Hash;NativeUpscalerSymbols=@('NVSDK_NGX','xess');Files=$files}
$audit | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$root\build-hashes.json"
$audit | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$PSScriptRoot\..\out\task117\package-audit.json"
$audit | Select-Object Package,Zip,SHA256,Entries,DevTools,RouteSHA256 | Format-List
