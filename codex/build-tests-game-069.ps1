param([ValidateSet('tests','game')][string]$Target='tests')
$ErrorActionPreference='Continue'
$taskEnv = cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" & set'
foreach ($line in $taskEnv) {
  if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process') }
}
$env:PATH='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin;'+$env:PATH
$workspace=(Resolve-Path "$PSScriptRoot/..").Path
if ($Target -eq 'tests') {
  cmake -S "$workspace/native-renderer/tests/uipaint-069" -B "$workspace/kk/out/uipaint-tests-069" -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=F:/KK-native-renderer/sdk-vulkan-upscalers-037/win-amd64 *> F:/KK-native-renderer/runs/task069-tests-configure.log
  if ($LASTEXITCODE) { throw 'Test configure failed' }
  cmake --build "$workspace/kk/out/uipaint-tests-069" -j 4 *> F:/KK-native-renderer/runs/task069-tests-build.log
  if ($LASTEXITCODE) { throw 'Test build failed' }
  Copy-Item F:/KK-native-renderer/sdk-vulkan-upscalers-037/win-amd64/bin/rexruntime.dll "$workspace/kk/out/uipaint-tests-069/"
  & "$workspace/kk/out/uipaint-tests-069/ui-paint-tests.exe" *> F:/KK-native-renderer/runs/task069-tests.log
  if ($LASTEXITCODE) { throw 'Presenter scheduling tests failed' }
} else {
  Set-Location "$workspace/kk"
  cmake --preset kk-dev-vulkan *> F:/KK-native-renderer/runs/task069-game-configure.log
  if ($LASTEXITCODE) { throw 'Game configure failed' }
  cmake --build --preset kk-dev-vulkan -j 8 *> F:/KK-native-renderer/runs/task069-game-build.log
  if ($LASTEXITCODE) { throw 'Game build failed' }
}
