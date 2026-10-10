@echo off
rem Usage: build.bat [release|debug|release-spirv]   (same toolchain as kk\build.bat: VS 2022's clang + Ninja)
setlocal EnableDelayedExpansion
set PRESET=%1
if "%PRESET%"=="" set PRESET=release
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%VSPATH%\VC\Tools\Llvm\x64\bin;%PATH%
cd /d "%~dp0"
cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
