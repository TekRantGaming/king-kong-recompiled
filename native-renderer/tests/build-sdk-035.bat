@echo off
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
"C:\Users\TekRa\AppData\Local\Programs\Python\Python312\python.exe" -I "%~dp0build-sdk-035.py" "%~1"
