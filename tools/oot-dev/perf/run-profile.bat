@echo off
rem Performance profile: per-present CPU/fence timing (EDEN_DKB_FRAME_TRACE), GPU-thread time by
rem zone each second (EDEN_NX2_GPU_PROFILE) and IPC counts every 5 s (EDEN_NX2_IPC_SUMMARY).
rem Throttle stays at Lichii's default. Same settings as run-dev.bat otherwise.
set "BIN=%~dp0..\..\..\build\bin"
set "EDEN_NX2_PIPELINE_BUILD_TRACE=%BIN%\pipeline-build-trace.txt"
set "EDEN_NX2_ASYNC_DISCARD=1"
set "EDEN_NX2_IPC_SUMMARY=1"
set "EDEN_NX2_GPU_PROFILE=1"
set "EDEN_DKB_FRAME_TRACE=1"
rem Why the front's macro DMA reads wait for the back (MME_DMA_CAUSES is local test only).
set "EDEN_NX2_MME_DMA_STATS=1"
set "EDEN_NX2_MME_DMA_CAUSES=1"
if exist "%EDEN_NX2_PIPELINE_BUILD_TRACE%" move /y "%EDEN_NX2_PIPELINE_BUILD_TRACE%" "%BIN%\pipeline-build-trace.old.txt" >nul
start "" "%BIN%\lichii.exe"
