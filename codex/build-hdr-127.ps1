param([ValidateSet('sdk','game','presenter')][string]$Target='sdk')
$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path "$PSScriptRoot/..").Path
$taskEnv = cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" & set'
foreach ($line in $taskEnv) {
  if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
$env:PATH = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin;' + $env:PATH
if ($Target -eq 'sdk') {
  & 'C:\Users\TekRa\AppData\Local\Programs\Python\Python312\python.exe' -I "$taskRoot/native-renderer/tests/build-sdk-127.py"
  if ($LASTEXITCODE) { throw 'HDR SDK build failed' }
} elseif($Target -eq 'presenter') {
  $presenterBuild="$taskRoot/kk/out/hdr-presenter-127"
  cmake -S "$taskRoot/native-renderer/tests/uipaint-069" -B $presenterBuild -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=F:/KK-native-renderer/sdk-vulkan-upscalers-043/win-amd64
  if($LASTEXITCODE) { throw 'Presenter configure failed' }
  cmake --build $presenterBuild -j 2
  if($LASTEXITCODE) { throw 'Presenter build failed' }
  Copy-Item -LiteralPath 'F:/KK-native-renderer/sdk-vulkan-upscalers-043/win-amd64/bin/rexruntime.dll' -Destination $presenterBuild -Force
  & "$presenterBuild/ui-paint-tests.exe"
  if($LASTEXITCODE) { throw 'Presenter tests failed' }
} else {
  Push-Location "$taskRoot/kk"
  try {
    cmake --preset kk-dev-vulkan -DCMAKE_PREFIX_PATH=F:/KK-native-renderer/sdk-vulkan-upscalers-043/win-amd64 -DNR_FFX_SDK_DIR=F:/KK-native-renderer/sdk-vulkan-upscalers-043/fidelityfx -DNR_BUILD_TESTS=ON -DNR_HDR_SDK_SOURCE=F:/KK-native-renderer/sdk-src-upscalers-043 -DPython3_EXECUTABLE=C:/Users/TekRa/AppData/Local/Programs/Python/Python312/python.exe -DNR_DXC_DIR=F:/KK-native-renderer/downloads/dxc-v1.9.2609/dxc/bin/x64 -DNR_LIGHTING_DXC=F:/KK-native-renderer/downloads/dxc-v1.9.2609/dxc/bin/x64/dxc.exe -DNR_DXC_RUNTIME=F:/KK-native-renderer/downloads/dxc-v1.9.2609/dxc/bin/x64/dxcompiler.dll
    if ($LASTEXITCODE) { throw 'HDR configure failed' }
    cmake --build --preset kk-dev-vulkan -j 6
    if ($LASTEXITCODE) { throw 'HDR game/tests build failed' }
  } finally { Pop-Location }
}
