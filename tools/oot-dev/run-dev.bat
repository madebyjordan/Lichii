@echo off
rem Normal OoT test run with the pipeline build trace and async discard on.
set "BIN=%~dp0..\..\build\bin"
set "EDEN_NX2_PIPELINE_BUILD_TRACE=%BIN%\pipeline-build-trace.txt"
set "EDEN_NX2_ASYNC_DISCARD=1"
if exist "%EDEN_NX2_PIPELINE_BUILD_TRACE%" move /y "%EDEN_NX2_PIPELINE_BUILD_TRACE%" "%BIN%\pipeline-build-trace.old.txt" >nul
start "" "%BIN%\lichii.exe"
