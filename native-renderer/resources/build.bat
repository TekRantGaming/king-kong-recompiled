@echo off
rem Usage: build.bat [res-debug|res-release]   (configure, build, run the unit tests)
setlocal EnableDelayedExpansion
set PRESET=%1
if "%PRESET%"=="" set PRESET=res-release
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%VSPATH%\VC\Tools\Llvm\x64\bin;%PATH%
cd /d "%~dp0"
cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
ctest --preset %PRESET% || exit /b 1
