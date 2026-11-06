@echo off
rem Shared setup for both A/B runs, so they start from the same state.
tasklist /FI "IMAGENAME eq lichii.exe" | find /i "lichii.exe" >nul && (
    echo Close Lichii first, then run this again.
    pause
    exit /b 1
)
set "SD=%APPDATA%\eden\cache\shader\0400486024dec000"
rem 1. Same pipeline lists as when the snapshot was taken.
copy /y "%~dp0snapshot\vulkan.fork-v53.bin" "%SD%\" >nul || exit /b 1
copy /y "%~dp0snapshot\vulkan_used.fork-v53.bin" "%SD%\" >nul || exit /b 1
if exist "%SD%\vulkan_pipelines.fork-v53.bin" del /q "%SD%\vulkan_pipelines.fork-v53.bin"
rem 2. No compiled shaders left in AMD's driver cache.
if exist "%LOCALAPPDATA%\AMD\VkCache" rd /s /q "%LOCALAPPDATA%\AMD\VkCache"
mkdir "%LOCALAPPDATA%\AMD\VkCache"
exit /b 0
