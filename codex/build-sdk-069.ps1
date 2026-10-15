$ErrorActionPreference='Continue'
$taskEnv = cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" & set'
foreach ($line in $taskEnv) {
  if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process') }
}
$env:PATH='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin;'+$env:PATH
Set-Location F:/KK-native-renderer/sdk-src-upscalers-037
# Build-only adjustment in the copied glslang tree: every Python script must
# run with the required interpreter and isolation flag.
$glslang='thirdparty/glslang/CMakeLists.txt'
$glslangText=[IO.File]::ReadAllText((Resolve-Path $glslang))
$glslangText=$glslangText.Replace('COMMAND ${PYTHON_EXECUTABLE} "${GLSLANG_BUILD_INFO_PY}"','COMMAND ${PYTHON_EXECUTABLE} -I "${GLSLANG_BUILD_INFO_PY}"')
[IO.File]::WriteAllText((Resolve-Path $glslang),$glslangText)
cmake --preset win-amd64 -DPYTHON_EXECUTABLE=C:/Users/TekRa/AppData/Local/Programs/Python/Python312/python.exe -DREXGLUE_BUILD_TESTS=OFF -DREXGLUE_USE_VULKAN=ON -DREXGLUE_USE_D3D12=ON -DREXGLUE_ENABLE_FIDELITYFX=OFF -DCMAKE_INSTALL_PREFIX=F:/KK-native-renderer/sdk-vulkan-upscalers-037/win-amd64 *> F:/KK-native-renderer/runs/task069-sdk-configure.log
if ($LASTEXITCODE) { throw 'SDK configure failed' }
cmake --build --preset win-amd64-release -j 8 *> F:/KK-native-renderer/runs/task069-sdk-build.log
if ($LASTEXITCODE) { throw 'SDK build failed' }
cmake --install out/build/win-amd64 --config Release *> F:/KK-native-renderer/runs/task069-sdk-install.log
if ($LASTEXITCODE) { throw 'SDK install failed' }
# The SDK installer unconditionally registers its prefix globally. Undo only
# this task's value so other worktrees keep their previous package discovery.
$registry='HKCU:/Software/Kitware/CMake/Packages/rexglue'
$value='3711204eeace593bbc0d2fd4a9d098c5'
if ((Get-ItemPropertyValue $registry $value -ErrorAction SilentlyContinue) -eq 'F:/KK-native-renderer/sdk-vulkan-upscalers-037/win-amd64') {
  Remove-ItemProperty -LiteralPath $registry -Name $value
}
