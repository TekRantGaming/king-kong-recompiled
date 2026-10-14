@echo off
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
"%VSPATH%\VC\Tools\Llvm\x64\bin\clang++.exe" -std=c++20 -I"%~dp0.." "%~dp0..\backend\tests\memory_budget_tests.cpp" -o "%~dp0..\..\kk\out\build\kk-dev-vulkan\nr_memory036.exe" || exit /b 1
"%~dp0..\..\kk\out\build\kk-dev-vulkan\nr_memory036.exe"
