@echo off
setlocal
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "TASKROOT=%~dp0..\.."
set "TESTOUT=F:\KK-native-renderer\runs\task043-unit"
if not exist "%TESTOUT%" mkdir "%TESTOUT%"
cl /nologo /std:c++20 /utf-8 /EHsc /DFMT_HEADER_ONLY /DSPDLOG_FMT_EXTERNAL /DKK_VERSION=\"test043\" /I"%TASKROOT%\kk\src" /I"%TASKROOT%\tools\rexglue-vulkan\win-amd64\include" "%~dp0diagnostics-043.cpp" "%TASKROOT%\kk\src\diagnostics.cpp" /Fe:"%TESTOUT%\diagnostics-test.exe" /Fo:"%TESTOUT%\\" /link dxgi.lib > "%TESTOUT%\build.log" 2>&1
if errorlevel 1 (
  type "%TESTOUT%\build.log"
  exit /b 1
)
"%TESTOUT%\diagnostics-test.exe" "%TESTOUT%\evidence-%RANDOM%"
