@echo off
setlocal
set PATH=C:\Windows\System32;C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "CXX=%VSPATH%\VC\Tools\Llvm\x64\bin\clang-cl.exe"
set "NR=%~dp0..\.."
set "NR_CXX=%NR:\=/%"
set "OUT=%NR%\..\kk\out\build\kk-dev-vulkan"
if not exist "%OUT%\native-renderer\backend\nr_draw.lib" exit /b 1
"%CXX%" /std:c++20 /MD /EHsc -I"%NR%" -I"%NR%\resources\include" "%~dp0cpu_paths_tests.cpp" "%OUT%\native-renderer\backend\nr_draw.lib" "%OUT%\native-renderer\kknr_resources.lib" -o "%OUT%\nr_cpu_paths_tests.exe" || exit /b 1
"%OUT%\nr_cpu_paths_tests.exe" || exit /b 1
for %%t in (draw frame_log ao_rules upscale vulkan_requirements) do call :test %%t || exit /b 1
exit /b 0
:test
"%CXX%" /std:c++20 /MD /EHsc -DNR_SOURCE_DIR="\"%NR_CXX%\"" -I"%NR%" "%~dp0%1_tests.cpp" "%OUT%\native-renderer\backend\nr_draw.lib" "%OUT%\native-renderer\hooks\nr_hooks.lib" -o "%OUT%\nr_%1_tests.exe" || exit /b 1
"%OUT%\nr_%1_tests.exe" || exit /b 1
exit /b 0
