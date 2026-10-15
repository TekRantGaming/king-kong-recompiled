@echo off
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VSPATH%\VC\Tools\Llvm\x64\bin;%PATH%"
cd /d "%~dp0..\.."
cmake -S native-renderer -B out/task036-tests -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DNR_BUILD_PLUGIN=ON "-DCMAKE_PREFIX_PATH=%CD%/tools/rexglue-vulkan/win-amd64" -DNR_FORCE_VULKAN=ON -DNR_VULKAN_HEADERS=F:/KK-native-renderer/downloads/vulkan-sdk-latest/sdk/Include -DNR_BUILD_TESTS=ON -DPython3_EXECUTABLE=C:/Users/TekRa/AppData/Local/Programs/Python/Python312/python.exe -DNR_DXC_DIR=F:/KK-native-renderer/downloads/dxc-v1.9.2609/dxc/bin/x64 || exit /b 1
rem Several test targets copy the same SDK DLLs; serialize their post-build copies.
cmake --build out/task036-tests --parallel 1
