@echo off
rem Performance test: counts IStorage reads every 5 s (EDEN_NX2_IPC_SUMMARY) so freezes can be
rem matched to file-read bursts. Same settings as run-dev.bat otherwise.
rem Throttle OFF: EDEN_NX2_FS_READ_MBPS=0 (from #278).
set "BIN=%~dp0..\..\..\build\bin"
set "EDEN_NX2_PIPELINE_BUILD_TRACE=%BIN%\pipeline-build-trace.txt"
set "EDEN_NX2_ASYNC_DISCARD=1"
set "EDEN_NX2_IPC_SUMMARY=1"
set "EDEN_NX2_FS_READ_MBPS=0"
if exist "%EDEN_NX2_PIPELINE_BUILD_TRACE%" move /y "%EDEN_NX2_PIPELINE_BUILD_TRACE%" "%BIN%\pipeline-build-trace.old.txt" >nul
start "" "%BIN%\lichii.exe"
