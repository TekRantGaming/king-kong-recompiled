@echo off
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%VSPATH%\VC\Tools\Llvm\x64\bin;%PATH%
cd /d "%~dp0..\..\kk"
cmake --preset kk-dev-vulkan -DCMAKE_C_COMPILER="%VSPATH%/VC/Tools/Llvm/x64/bin/clang.exe" -DCMAKE_CXX_COMPILER="%VSPATH%/VC/Tools/Llvm/x64/bin/clang++.exe" -Drexglue_DIR="%CD%/../tools/rexglue-vulkan/win-amd64/lib/cmake/rexglue" -DNR_DLSS_SDK_DIR=F:/KK-native-renderer/downloads/nvidia-dlss-sdk-v310.9.1/x/NVIDIA-DLSS-d8339a2 -DNR_FFX_SDK_DIR=F:/KK-native-renderer/downloads/amd-fidelityfx-sdk-v1.1.4/x -DNR_DXC_RUNTIME=F:/KK-native-renderer/downloads/dxc-v1.9.2609/dxc/bin/x64/dxcompiler.dll || exit /b 1
cmake --build --preset kk-dev-vulkan || exit /b 1
