@echo off
rem King Kong Recompiled: comparison capture (private test build, task 354).
rem Starts the game with the comparison tools, fullscreen at the desktop resolution.
cd /d "%~dp0"
start "" "%~dp0king_kong.exe" --kk_compare_capture=true --fullscreen=true --kk_fullscreen_mode=desktop
