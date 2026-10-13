@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang++.exe" -std=c++20 -I native-renderer native-renderer/backend/tests/motion_pipeline_tests.cpp native-renderer/backend/pipeline_cache.cpp -o kk/out/task017-motion-tests.exe || exit /b 1
kk\out\task017-motion-tests.exe
