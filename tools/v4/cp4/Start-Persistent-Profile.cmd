@echo off
setlocal
set "NSIGHT_EXE=C:\Program Files\NVIDIA Corporation\Nsight Systems 2026.5.1\target-windows-x64\nsys.exe"
if not exist "%NSIGHT_EXE%" (
    echo Nsight Systems was not found at the installed path.
    pause
    exit /b 1
)
rem Uses exactly the optimized candidate controls. No self-elevation.
call "%~dp0Start-Persistent-Vulkan.cmd" --nsight "%NSIGHT_EXE%" --profile-cpu --profile-seconds 20 %*
exit /b %ERRORLEVEL%
