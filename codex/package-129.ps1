param([switch]$StageOnly)
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path "$PSScriptRoot/..").Path
$build="$workspace/kk/out/build/kk-beta"
$record="$workspace/kk/out/task129/package-root.txt"
if(Test-Path -LiteralPath $record){$root=(Get-Content -LiteralPath $record -Raw).Trim()}
else {
 $root='F:\KK-native-renderer\testbuilds\freecam-test-'+(Get-Date -Format MMdd-HHmm)
 if(Test-Path -LiteralPath $root){throw 'Destination exists'}
 New-Item -ItemType Directory -Path "$root/KingKong" | Out-Null
 $root | Set-Content -LiteralPath $record
}
$stage="$root/KingKong"
$baseline='F:\KK-native-renderer\testbuilds\beta6-test3-1015-1729\KingKong'
& robocopy $baseline $stage /E /XF TEST-CHECKLIST.txt /NFL /NDL /NJH /NJS | Out-Null
if($LASTEXITCODE -gt 7){throw 'Baseline stage failed'}
& robocopy "$workspace/kk/assets" "$stage/game" /E /COPY:DAT /NFL /NDL /NJH /NJS | Out-Null
if($LASTEXITCODE -gt 7){throw 'Game assets stage failed'}
$movieSource='C:\Users\TekRa\Desktop\kk_upscaled_movies'
if(@(Get-ChildItem -LiteralPath $movieSource -Filter '*.mp4' -File).Count -ne 42){throw 'Expected 42 supplied movies'}
& robocopy $movieSource "$stage/movies" '*.mp4' /COPY:DAT /NFL /NDL /NJH /NJS | Out-Null
if($LASTEXITCODE -gt 7){throw 'Movie stage failed'}
@'
PRIVATE King Kong free-camera trailer build

This archive contains the full game assets and 42 supplied PS2 movies.
PRIVATE: for the trailer editor and owner/coordinator testing only.
NOT FOR REDISTRIBUTION. This is a test build, not a public release.

Extract the complete KingKong folder. Start king_kong.exe, choose any chapter,
then use the keyboard camera controls in FREECAM.txt. F9 and F10 do not need
the F8 panel. PS2 cutscenes are enabled and read the movies folder beside
the executable; keep that folder with the game when moving this build.
Movies play at normal adjacent story transitions, not at chapter-select,
boot, restart or arbitrary chapter jumps. The white placeholder is skipped.

Includes the recorded benchmark route, graphics quick wins, GI/UI fixes,
SMAA, PS2 movie playback and the free camera. The unfinished hfr-beta6,
tex-packs and hdr branches are excluded. GI follows the graphics preset;
other new lighting effects remain off by default. Vulkan is primary and
DirectX 12 is also available. All upscalers are included.

F11 freeze is unavailable: the current game pause path stops scene rendering,
and a zero time scale has not been proven safe. The world continues running
while the camera is detached. Characters and story events can still advance.
Free camera does not move the player or request new streamed areas. Flying
beyond the loaded level can expose missing geometry; return with F9.

Developer tools are OFF in this package. Version text remains 2.0.0-beta.5.
This is the private task-129 integration build. See FREECAM.txt for controls.
'@ | Set-Content -LiteralPath "$stage/README.txt" -Encoding ASCII
@'
FREE CAMERA - PRIVATE TRAILER BUILD

F9              Detach camera / return to the current normal game camera
W / S           Fly forward / backward along the camera direction
A / D           Strafe left / right
Mouse           Look around (uses the game's mouse sensitivity/invert setting)
Space or E      Move up along the world's vertical axis
Ctrl or Q       Move down along the world's vertical axis
Shift           Move 4 times faster while held
Alt             Move 4 times slower while held
Mouse wheel     Increase / decrease flight speed (25% per notch)
F10             Hide / restore game HUD, port overlays, toasts and FPS counter
F11             Unavailable: world freeze was skipped for safety

While F9 is enabled the player receives no keyboard, mouse or controller
input. The world keeps running. F9 restores the live normal game camera;
the player has not teleported. A level transition disables free camera.
Changing chapters or restarting is done with free camera off.

F2 (FPS), F6 (capture), F7 (comparison), F8 (graphics panel) keep their keys.
Close the F8 panel before flying. F10 is temporary and does not save settings.
Free camera is keyboard-only and is available in gameplay, not in menus.
Focus must be in the game window to use the physical keyboard and mouse.
'@ | Set-Content -LiteralPath "$stage/FREECAM.txt" -Encoding ASCII
@'
# Private task-129 build. Relative movie paths resolve beside the executable.
kk_ps2_cutscenes = true
kk_ps2_movie_folder = "movies"
kk_update_channel = "beta"
kk_check_updates = false
'@ | Set-Content -LiteralPath "$stage/king_kong.toml" -Encoding ASCII
if($StageOnly){Write-Output "Staged assets: $stage";exit 0}
$cache=Get-Content -LiteralPath "$build/CMakeCache.txt" -Raw
if($cache -notmatch '(?m)^KK_DEV_TOOLS:BOOL=OFF\r?$'){throw 'Packaged player must have developer tools OFF'}
Copy-Item -LiteralPath "$build/king_kong.exe" -Destination $stage
Get-ChildItem -LiteralPath $build -Filter '*.dll' | Copy-Item -Destination $stage
Copy-Item -LiteralPath "$workspace/kk/THIRD-PARTY-NOTICES.txt" -Destination $stage
$links=@('kk/assets','kk/generated/default','tools/rexglue','tools/rexglue-vulkan','tools/upscaler-sdks/dlss','tools/upscaler-sdks/fsr3','tools/upscaler-sdks/fsr4','tools/upscaler-sdks/xess')
$linkAudit=@(foreach($relative in $links){
 $item=Get-Item -LiteralPath "$workspace/$relative"
 if(!$item.Target){throw "Expected worktree link: $relative"}
 foreach($target in $item.Target){if($target -like 'F:\KK-native-renderer\wt\*' -or !(Test-Path -LiteralPath $target)){throw "Broken/temporary link: $target"}}
 [pscustomobject]@{Path=$relative;Target=$item.Target}
})
$strings='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\llvm-strings.exe'
$nativeStrings=& $strings "$stage/rexgpu-native.dll"
foreach($s in @('NVSDK_NGX','xess')){if(!($nativeStrings | Select-String -SimpleMatch $s)){throw "Missing upscaler symbol $s"}}
foreach($dll in @('nvngx_dlss.dll','libxess.dll','amd_fidelityfx_dx12.dll','amd_fidelityfx_vk.dll','amd_fidelityfx_loader_dx12.dll','amd_fidelityfx_upscaler_dx12.dll','dxcompiler.dll')){
 if(!(Test-Path -LiteralPath "$stage/$dll")){throw "Missing upscaler/runtime $dll"}
}
if(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object {$_.Name -match '^rexgpu-(xenos|classic)\.' -or $_.FullName -match '[\\/]native-shaders[\\/]'}){throw 'Classic DLL or prepared shader cache in package'}
foreach($file in Get-ChildItem -LiteralPath "$stage/movies" -File){
 if((Get-FileHash -LiteralPath $file.FullName).Hash -ne (Get-FileHash -LiteralPath "$movieSource/$($file.Name)").Hash){throw "Movie hash mismatch: $($file.Name)"}
}
$zip="$root/KingKong-freecam-private-windows-x64.zip"
Push-Location $root
try {& 'C:\Windows\System32\tar.exe' -a -c -f $zip KingKong;if($LASTEXITCODE){throw 'Windows tar failed'}}finally{Pop-Location}
$s=[IO.File]::OpenRead($zip)
try {if($s.ReadByte()-ne 80 -or $s.ReadByte()-ne 75){throw 'Expected PK signature'}}finally{$s.Dispose()}
$entries=@(& 'C:\Windows\System32\tar.exe' -t -f $zip)
if($LASTEXITCODE -or @($entries | Where-Object {$_ -match '^KingKong/movies/.*\.mp4$'}).Count -ne 42 -or $entries -notcontains 'KingKong/benchmark-route.kkbr'){throw 'Archive inventory failed'}
$hash=(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
"$hash  KingKong-freecam-private-windows-x64.zip" | Set-Content -LiteralPath "$root/SHA256SUMS.txt" -Encoding ASCII
$files=@(Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object {
 [pscustomobject]@{Path=$_.FullName.Substring($stage.Length+1);Bytes=$_.Length;SHA256=(Get-FileHash -LiteralPath $_.FullName).Hash}
})
[pscustomobject]@{Zip=$zip;SHA256=$hash;Entries=$entries.Count;DevTools='OFF';Links=$linkAudit;Files=$files} |
 ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$workspace/kk/out/task129/package-audit.json"
$zip | Set-Content -LiteralPath 'F:\KK-native-renderer\codex\FREECAM-READY.txt' -Encoding ASCII
Write-Output "READY $zip SHA256=$hash"
