param([string]$Build='F:\KK-native-renderer\wt\ps2-movies\kk\out\task118\player')
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path "$PSScriptRoot/..").Path
$cache=Get-Content -LiteralPath "$Build\CMakeCache.txt" -Raw
if($cache -notmatch '(?m)^KK_DEV_TOOLS:BOOL=OFF\r?$' -or $cache -notmatch 'sdk-vulkan-upscalers-041/win-amd64'){
 throw 'Expected the private player build, developer tools OFF, permanent SDK 041'
}
$links=@('kk\assets','kk\generated\default','tools\rexglue','tools\rexglue-vulkan','tools\upscaler-sdks\dlss','tools\upscaler-sdks\fsr3','tools\upscaler-sdks\fsr4','tools\upscaler-sdks\xess')
$linkAudit=@(foreach($relative in $links){
 $item=Get-Item -LiteralPath "$workspace\$relative"
 if(!$item.Target -or !($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw "Expected a permanent worktree link: $relative"}
 foreach($target in $item.Target){if($target -like 'F:\KK-native-renderer\wt\*' -or !(Test-Path -LiteralPath $target)){throw "Non-permanent or missing link target: $target"}}
 [pscustomobject]@{Path=$relative;Target=$item.Target}
})
$root='F:\KK-native-renderer\testbuilds\ps2-movies-test-'+(Get-Date -Format MMdd-HHmm)
if(Test-Path -LiteralPath $root){throw "Package destination already exists: $root"}
$stage="$root\KingKong"
New-Item -ItemType Directory -Path $stage | Out-Null
$base='F:\KK-native-renderer\testbuilds\beta6-test3-1015-1729\KingKong'
& robocopy $base $stage /E /XF 'TEST-CHECKLIST.txt' /NFL /NDL /NJH /NJS | Out-Null
if($LASTEXITCODE -gt 7){throw 'Baseline copy failed'}
Copy-Item -LiteralPath "$Build\king_kong.exe","$Build\rexgpu-native.dll","$Build\rexruntime.dll" -Destination $stage
$howto=@'
PRIVATE King Kong PS2 movies test build - task 118

This is based on Beta 6 test build 3. It is for local owner/coordinator
testing only. Do not redistribute it. The inherited version string remains
2.0.0-beta.5. Developer tools are disabled in this package.

No movies or other game files are included. Keep the supplied 42 MP4 files
in C:\Users\TekRa\Desktop\kk_upscaled_movies. The default uses Windows'
Desktop folder plus kk_upscaled_movies, so it also follows a redirected
Desktop. It reads the files there without moving or modifying them.

Gameplay > Play PS2 cutscenes is on by default in this private build.
Gameplay > PS2 movie folder chooses another folder. The corresponding
settings are kk_ps2_cutscenes and kk_ps2_movie_folder in king_kong.toml;
an empty movie-folder setting uses the Desktop default.

Play through the story to a following mission. A movie is inserted before
the next level loads. Boot, Continue, death/restart, chapter-practice return,
menus and arbitrary chapter jumps do not insert movies. A missing file or
unmapped destination leaves the normal load unchanged. The single white
placeholder is automatically suppressed. The separate Back to Kong Island
clip has no transition in the 42-chapter story list.

The bottom-right Skip prompt stays visible. Press the displayed controller
button (A/Cross) or Esc; Enter/Back also work. Skip immediately releases the
normal next-mission loader. Movies keep their own timestamps and aspect
ratio at every selected frame cap. No interpolated frames are generated.

The supplied soundtrack is mixed. Its gain uses Master volume multiplied
by the lower of Music and Effects volume, so muting either category mutes
the clip. Existing in-engine cutscenes and the original story movies remain
the game's own. This build adds no third-party media runtime.

Owner checks: story order, perceived audio/video sync, speaker volume,
physical controller/keyboard skip, changed movie-folder persistence,
ultrawide/fullscreen appearance and continued play after each transition.
See F:\KK-native-renderer\codex\REPORT-118.md for the exact automated tests,
private mapping confidence and remaining verification limits.
'@
$howto | Set-Content -LiteralPath "$stage\HOW-TO.txt" -Encoding ASCII
$dlls=@('nvngx_dlss.dll','libxess.dll','amd_fidelityfx_dx12.dll','amd_fidelityfx_vk.dll','amd_fidelityfx_loader_dx12.dll','amd_fidelityfx_upscaler_dx12.dll','dxcompiler.dll')
foreach($name in $dlls){if(!(Test-Path -LiteralPath "$stage\$name")){throw "Missing upscaler/runtime: $name"}}
$strings='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\llvm-strings.exe'
$nativeStrings=& $strings "$stage\rexgpu-native.dll"
foreach($symbol in @('NVSDK_NGX','xess')){if(!($nativeStrings | Select-String -SimpleMatch $symbol)){throw "Missing upscaler symbols: $symbol"}}
if(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object {$_.Extension -in '.mp4','.bf','.xex','.wmv' -or $_.Name -match '^rexgpu-(xenos|classic)\.'}){
 throw 'Game/movie data or Classic plugin appeared in staging'
}
if(Test-Path -LiteralPath "$stage\user\cache\native-shaders"){throw 'Prepared native-shader cache in staging'}
$zip="$root\KingKong-ps2-movies-private-windows-x64.zip"
Push-Location $root
try{& 'C:\Windows\System32\tar.exe' -a -c -f $zip KingKong;if($LASTEXITCODE){throw 'Windows tar failed'}}
finally{Pop-Location}
$stream=[IO.File]::OpenRead($zip)
try{if($stream.ReadByte()-ne 80 -or $stream.ReadByte()-ne 75){throw 'Missing PK signature'}}finally{$stream.Dispose()}
$entries=@(& 'C:\Windows\System32\tar.exe' -t -f $zip)
if($LASTEXITCODE -or $entries -notcontains 'KingKong/HOW-TO.txt' -or $entries -match '\.(mp4|bf|xex|wmv)$|native-shaders|rexgpu-(classic|xenos)\.'){
 throw 'Archive inventory failed'
}
$hash=(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
"$hash  KingKong-ps2-movies-private-windows-x64.zip" | Set-Content -LiteralPath "$root\SHA256SUMS.txt" -Encoding ASCII
$files=@(Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object {
 [pscustomobject]@{Path=$_.FullName.Substring($stage.Length+1);Bytes=$_.Length;SHA256=(Get-FileHash -LiteralPath $_.FullName).Hash}
})
# Check that the rest of test build 3 was retained byte for byte.
foreach($file in Get-ChildItem -LiteralPath $base -Recurse -File){
 $relative=$file.FullName.Substring($base.Length+1)
 if($relative -in @('king_kong.exe','rexgpu-native.dll','rexruntime.dll','TEST-CHECKLIST.txt')){continue}
 if((Get-FileHash -LiteralPath "$stage\$relative").Hash -ne (Get-FileHash -LiteralPath $file.FullName).Hash){throw "Baseline file changed: $relative"}
}
$audit=[pscustomobject]@{Package=$root;Zip=$zip;SHA256=$hash;Entries=$entries.Count;DevTools='OFF';UpscalerSymbols=@('NVSDK_NGX','xess');PermanentLinks=$linkAudit;Files=$files}
$audit | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$root\build-hashes.json"
$audit | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$workspace\kk\out\task118\package-audit.json"
$audit | Select-Object Package,Zip,SHA256,Entries,DevTools | Format-List
