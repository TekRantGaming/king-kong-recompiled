@echo off
setlocal
set PATH=C:\Windows\System32;C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "OUT=%~dp0..\..\kk\out\build\kk-dev-vulkan"
set "NR_MOTION_TEST_DIR=%OUT%"
if not exist "%OUT%" mkdir "%OUT%" || exit /b 1
"%VSPATH%\VC\Tools\Llvm\x64\bin\clang++.exe" -std=c++20 -I"%~dp0.." "%~dp0..\backend\tests\motion_pipeline_tests.cpp" "%~dp0..\backend\pipeline_cache.cpp" -o "%OUT%\task017-motion-tests.exe" || exit /b 1
"%OUT%\task017-motion-tests.exe"
