@echo off
rem Normal OoT test run with the pipeline build trace and async discard on.
set "BIN=%~dp0..\..\build\bin"
set "EDEN_NX2_PIPELINE_BUILD_TRACE=%BIN%\pipeline-build-trace.txt"
set "EDEN_NX2_ASYNC_DISCARD=1"
rem Copy GPU-written buffers back at fences instead of stalling CPU reads (#259):
rem ~70 -> ~5 stalls/s, ~+10% fps on RX 6700 XT in Castle Town.
set "EDEN_NX2_PREFLUSH=1"
if exist "%EDEN_NX2_PIPELINE_BUILD_TRACE%" move /y "%EDEN_NX2_PIPELINE_BUILD_TRACE%" "%BIN%\pipeline-build-trace.old.txt" >nul
start "" "%BIN%\lichii.exe"
