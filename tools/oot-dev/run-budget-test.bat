@echo off
rem Like run-dev.bat, with the Switch 2 pipeline budget forced to 1500 to exercise eviction.
set "BIN=%~dp0..\..\build\bin"
set "EDEN_NX2_PIPELINE_BUILD_TRACE=%BIN%\pipeline-build-trace.txt"
set "EDEN_NX2_ASYNC_DISCARD=1"
set "EDEN_NX2_TEST_PIPELINE_BUDGET=1500"
if exist "%EDEN_NX2_PIPELINE_BUILD_TRACE%" move /y "%EDEN_NX2_PIPELINE_BUILD_TRACE%" "%BIN%\pipeline-build-trace.old.txt" >nul
start "" "%BIN%\lichii.exe"
