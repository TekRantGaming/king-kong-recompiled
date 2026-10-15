@echo off
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VSPATH%\VC\Tools\Llvm\x64\bin;%PATH%"
set "DXCROOT=F:/KK-native-renderer/downloads/dxc-v1.9.2609/dxc"
cd /d "%~dp0..\..\kk"
cmake --preset kk-dev-vulkan -DNR_BUILD_TESTS=ON -DNR_DXC_DIR="%DXCROOT%" -DNR_LIGHTING_DXC="%DXCROOT%/bin/x64/dxc.exe" -DPython3_EXECUTABLE="C:/Users/TekRa/AppData/Local/Programs/Python/Python312/python.exe" || exit /b 1
cmake --build --preset kk-dev-vulkan || exit /b 1
ctest --test-dir out/build/kk-dev-vulkan --output-on-failure || exit /b 1
cmake --preset kk-beta -DNR_DXC_DIR="%DXCROOT%" || exit /b 1
cmake --build --preset kk-beta || exit /b 1
