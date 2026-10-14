@echo off
setlocal
if "%~1"=="" exit /b 2
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%~1" mkdir "%~1"
"%VSPATH%\VC\Tools\Llvm\x64\bin\clang++.exe" -std=c++20 "%~dp0startup-recovery-039.cpp" -o "%~1\startup-recovery-tests.exe" || exit /b 1
"%~1\startup-recovery-tests.exe" "%~1\state"
