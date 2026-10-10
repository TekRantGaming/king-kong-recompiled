@echo off
rem Usage: build.bat [test]   (same toolchain as kk\build.bat: VS 2022's clang + Ninja)
rem Needs the SDK clone at C:\rexsrc (fmt, xxHash, dxcapi.h). DXC: set KK_DXC_DIR to a DXC release
rem folder (bin\x64\dxcompiler.dll and dxil.dll), or have dxcompiler.dll and dxil.dll on PATH.
rem spirv-val for the tests: set KK_SPIRV_VAL to it (or have it on PATH or in the Vulkan SDK).
rem docs\shaders.md lists where both come from.
setlocal EnableDelayedExpansion
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%VSPATH%\VC\Tools\Llvm\x64\bin;%PATH%
cd /d "%~dp0"
cmake --preset release || exit /b 1
cmake --build --preset release || exit /b 1
if "%1"=="test" ctest --preset release || exit /b 1
