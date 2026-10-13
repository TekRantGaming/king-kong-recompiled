@echo off
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VSPATH%\VC\Tools\Llvm\x64\bin;%PATH%"
cd /d "%~dp0..\.."
cmake -S native-renderer -B out/task016-tests -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=tools/rexglue-vulkan/win-amd64 -DNR_FORCE_VULKAN=ON -DNR_VULKAN_HEADERS=F:/KK-native-renderer/downloads/vulkan-sdk-latest/sdk/Include -DNR_BUILD_TESTS=ON -DNR_DXC_DIR=F:/KK-native-renderer/downloads/dxc-v1.9.2609/dxc/bin/x64 || exit /b 1
cmake --build out/task016-tests --target nr_ao_rules_tests nr_gtao_tests nr_game_ao_tests nr_upscale_tests || exit /b 1
ctest --test-dir out/task016-tests -R "(ao_rules|gtao|game_ao|upscale_tests)" --output-on-failure
