@echo off
rem A/B test, run A: normal driver optimization.
rem Restores the same pipeline lists, clears AMD's Vulkan shader cache, then starts Lichii.
call "%~dp0prepare.bat" || exit /b 1
set "EDEN_NX2_PIPELINE_BUILD_TRACE=%~dp0trace-A.txt"
set "EDEN_NX2_ASYNC_DISCARD=1"
if exist "%EDEN_NX2_PIPELINE_BUILD_TRACE%" del /q "%EDEN_NX2_PIPELINE_BUILD_TRACE%"
start "" "%~dp0..\..\..\build\bin\lichii.exe"
