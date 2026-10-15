param([ValidateSet('tests','pixels')][string]$Mode='tests', [string]$InputPixels='', [string]$OutputPrefix='')
$ErrorActionPreference='Stop'
$taskRoot=(Resolve-Path "$PSScriptRoot/..").Path
. "$PSScriptRoot/game-slot.ps1"
$guard=Wait-GameSlot -Task '127'
try {
  $build="$taskRoot/kk/out/build/kk-dev-vulkan"
  $env:PATH="$build;"+$env:PATH
  $env:KKSHADERS_DXC='F:/KK-native-renderer/downloads/dxc-v1.9.2609/dxc/bin/x64/dxcompiler.dll'
  if($Mode -eq 'tests') {
    ctest --test-dir $build --output-on-failure -j 1
  } else {
    & "$build/native-renderer/backend/tests/nr_hdr_output_tests.exe" $InputPixels $OutputPrefix
  }
  if($LASTEXITCODE) { throw "HDR verification failed: $Mode" }
} finally { Release-GameSlot $guard }
