@echo off
rem A/B test, run B: identical to run A, except pipelines with the tessellation shader
rem da3613e0fed1f0be are built with driver optimization off.
call "%~dp0prepare.bat" || exit /b 1
set "EDEN_NX2_PIPELINE_BUILD_TRACE=%~dp0trace-B.txt"
set "EDEN_NX2_ASYNC_DISCARD=1"
set "EDEN_NX2_NOOPT_SHADERS=da3613e0fed1f0be"
if exist "%EDEN_NX2_PIPELINE_BUILD_TRACE%" del /q "%EDEN_NX2_PIPELINE_BUILD_TRACE%"
start "" "%~dp0..\..\..\build\bin\lichii.exe"
