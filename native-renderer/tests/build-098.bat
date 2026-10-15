@echo off
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VSPATH%\VC\Tools\Llvm\x64\bin;%PATH%"
set CMAKE_BUILD_PARALLEL_LEVEL=4
cd /d "%~dp0..\.."
if "%1"=="game" goto game
cmake -S native-renderer -B kk/out/task098-tests -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%CD%/tools/rexglue-vulkan/win-amd64" -DNR_FORCE_VULKAN=ON -DNR_VULKAN_HEADERS=F:/KK-native-renderer/downloads/vulkan-sdk-latest/sdk/Include -DNR_BUILD_TESTS=ON -DPython3_EXECUTABLE=C:/Users/TekRa/AppData/Local/Programs/Python/Python312/python.exe -DNR_DXC_DIR=F:/KK-native-renderer/downloads/dxc-v1.9.2609/dxc/bin/x64 || exit /b 1
cmake --build kk/out/task098-tests || exit /b 1
exit /b 0
:game
call kk\build.bat kk-dev-vulkan
