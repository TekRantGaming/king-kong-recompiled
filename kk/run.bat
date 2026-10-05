@echo off
rem Launch the recompiled game. Extra args are passed through, e.g.:
rem   run.bat --okx_launcher=false  (skip the launcher)
rem   run.bat --log_level=debug
cd /d "%~dp0out\build\kk-relwithdebinfo"
start "" king_kong.exe --game_data_root="%~dp0assets" --log_file=run.log %*
