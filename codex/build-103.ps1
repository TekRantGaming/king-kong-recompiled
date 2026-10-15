param([ValidateSet('renderer','resources','shader-control')][string]$Target = 'renderer')
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path "$PSScriptRoot/..").Path.Replace('\', '/')
$out = "$workspace/out/task103"
New-Item -ItemType Directory -Path $out -Force | Out-Null
$taskEnv = cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" & set'
foreach ($line in $taskEnv) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
$env:PATH = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin;' + $env:PATH
$sdk = 'F:/KK-native-renderer/sdk-vulkan-upscalers-038/win-amd64'
$dxc = 'F:/KK-native-renderer/downloads/dxc-v1.9.2609/dxc'
if ($Target -eq 'renderer') {
    cmake -S "$workspace/native-renderer" -B "$out/renderer" -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$sdk" -DNR_BUILD_PLUGIN=ON -DNR_BUILD_TESTS=ON "-DNR_FFX_SDK_DIR=$workspace/tools/rexglue-vulkan/fidelityfx" "-DNR_DXC_DIR=$dxc" "-DNR_LIGHTING_DXC=$dxc/bin/x64/dxc.exe" "-DNR_DXC_RUNTIME=$dxc/bin/x64/dxcompiler.dll" -DPython3_EXECUTABLE=C:/Users/TekRa/AppData/Local/Programs/Python/Python312/python.exe *> "$out/renderer-configure.log"
    if ($LASTEXITCODE) { throw 'Renderer configure failed' }
    cmake --build "$out/renderer" --target nr_game_upscale_tests -j 4 *> "$out/renderer-build.log"
} elseif ($Target -eq 'resources') {
    cmake -S "$workspace/native-renderer/resources" -B "$out/resources" -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DKKNR_RESOURCES_TOOLS=OFF *> "$out/resources-configure.log"
    if ($LASTEXITCODE) { throw 'Resources configure failed' }
    cmake --build "$out/resources" -j 4 *> "$out/resources-build.log"
    if ($LASTEXITCODE) { throw 'Resources build failed' }
    ctest --test-dir "$out/resources" --output-on-failure *> "$out/resources-tests.log"
} else {
    # Use the exact released sources, with only the two Windows harness startup
    # corrections from bd4fb7f. No extraction, scheduling or translator changes.
    $src = "$out/beta5-src"
    if (!(Test-Path "$src/native-renderer/shaders/CMakeLists.txt")) {
        git -C $workspace archive --format=tar "--output=$out/beta5-shaders.tar" e890ece -- native-renderer/shaders native-renderer/thirdparty/XenosRecomp
        if ($LASTEXITCODE) { throw 'Baseline archive failed' }
        New-Item -ItemType Directory -Path $src -Force | Out-Null
        & 'C:/Windows/System32/tar.exe' -xf "$out/beta5-shaders.tar" -C $src
        if ($LASTEXITCODE) { throw 'Baseline extraction failed' }
        $random = "$src/native-renderer/shaders/tests/random_program.cpp"
        [IO.File]::WriteAllText($random, [IO.File]::ReadAllText($random).Replace('#include "random_program.h"', "#include `"random_program.h`"`n#include <bit>"))
        $gpu = "$src/native-renderer/shaders/tests/gpu_tests.cpp"
        [IO.File]::WriteAllText($gpu, [IO.File]::ReadAllText($gpu).Replace('std::setvbuf(stdout, nullptr, _IOLBF, 0);', 'std::setvbuf(stdout, nullptr, _IONBF, 0);'))
    }
    cmake -S "$src/native-renderer/shaders" -B "$out/shader-control" -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release "-DKKSHADERS_REXGLUE_DIR=$sdk" "-DKKSHADERS_DXC_DIR=$dxc/bin/x64" -DKKSHADERS_SPIRV_VAL=F:/KK-native-renderer/downloads/vulkan-sdk-latest/sdk/Bin/spirv-val.exe -DVulkan_INCLUDE_DIR=F:/KK-native-renderer/downloads/vulkan-sdk-latest/sdk/Include -DVulkan_LIBRARY=F:/KK-native-renderer/downloads/vulkan-sdk-latest/sdk/Lib/vulkan-1.lib *> "$out/shader-control-configure.log"
    if ($LASTEXITCODE) { throw 'Shader control configure failed' }
    cmake --build "$out/shader-control" --target kkshaders_gpu_tests -j 4 *> "$out/shader-control-build.log"
}
if ($LASTEXITCODE) { throw "$Target failed; see $out" }
Write-Output "$Target passed; logs: $out"
