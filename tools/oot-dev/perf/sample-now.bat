@echo off
rem Samples Lichii's GPU threads and guest CPU cores for 45 s while the game runs (LOCAL TEST ONLY).
rem Double-click once you're in the scene to profile and keep playing normally until it says done.
set "OUT=%~dp0sample-report-%DATE:~-4%%DATE:~3,2%%DATE:~0,2%-%TIME:~0,2%%TIME:~3,2%.txt"
set "OUT=%OUT: =0%"
python -I "%~dp0sample_threads.py" --threads GPUBack GPU CPUCore_* --hz 500 --seconds 45 --out "%OUT%"
echo.
echo Done. Report: %OUT%
pause
