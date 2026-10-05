@echo off
rem Double-click to build King Kong on this PC from your own Xbox 360 disc image.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-KingKong.ps1" %*
