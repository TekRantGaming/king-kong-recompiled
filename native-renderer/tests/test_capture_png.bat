@echo off
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSPATH=%%i"
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "ROOT=%~dp0..\.."
set "OUT=%ROOT%\kk\out\task029-png-tests"
if not exist "%OUT%" mkdir "%OUT%"
"%VSPATH%\VC\Tools\Llvm\x64\bin\clang-cl.exe" /std:c++latest /O2 /MD /EHsc /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I "%ROOT%\tools\rexglue-vulkan\win-amd64\include" "%~dp0test_capture_png.cpp" /Fo"%OUT%\test_capture_png.obj" /Fe"%OUT%\capture_png_tests.exe" /link windowscodecs.lib ole32.lib || exit /b 1
"%OUT%\capture_png_tests.exe" "%OUT%\synthetic.png" || exit /b 1
"C:\Users\TekRa\AppData\Local\Programs\Python\Python312\python.exe" -I "%~dp0test_capture_png.py" "%OUT%\synthetic.png"
exit /b %ERRORLEVEL%


