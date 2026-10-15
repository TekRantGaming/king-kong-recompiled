param([ValidateSet('dev','player','tests')][string]$Target='dev')
$ErrorActionPreference='Stop'
$workspace=(Resolve-Path "$PSScriptRoot/..").Path.Replace('\','/')
$out="$workspace/kk/out/task118"
New-Item -ItemType Directory -Path $out -Force | Out-Null
$taskEnv=cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" & set'
foreach($line in $taskEnv) {
 if($line -match '^([^=]+)=(.*)$') {[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}
}
$env:PATH='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin;'+$env:PATH
$build="$out/$(if($Target -eq 'player'){'player'}else{'dev'})"
$dev=if($Target -eq 'player'){'OFF'}else{'ON'}
cmake -S "$workspace/kk" -B $build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release "-DKK_DEV_TOOLS:BOOL=$dev" -DCMAKE_PREFIX_PATH=F:/KK-native-renderer/sdk-vulkan-upscalers-041/win-amd64 -DNR_FFX_SDK_DIR=F:/KK-native-renderer/sdk-vulkan-upscalers-041/fidelityfx -DPython3_EXECUTABLE=C:/Users/TekRa/AppData/Local/Programs/Python/Python312/python.exe -DNR_DXC_RUNTIME=F:/KK-native-renderer/downloads/dxc-v1.9.2609/dxc/bin/x64/dxcompiler.dll *> "$out/configure-$Target.log"
if($LASTEXITCODE){throw "Configure failed: $out/configure-$Target.log"}
if($Target -eq 'tests') {
 cmake --build $build --target kk-ps2-movie-tests -j 4 *> "$out/build-tests.log"
 if($LASTEXITCODE){throw 'Movie unit-test build failed'}
 ctest --test-dir $build -R '^ps2-movie-logic$' --output-on-failure *> "$out/tests.log"
 if($LASTEXITCODE){throw 'Movie unit tests failed'}
} else {
 cmake --build $build --target king_kong -j 4 *> "$out/build-$Target.log"
 if($LASTEXITCODE){throw "Build failed: $out/build-$Target.log"}
}
"$Target passed: $build"
