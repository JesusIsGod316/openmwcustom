@echo off
setlocal
rem Explicit opt-in profiling launch. No self-elevation or game termination.
set "NSIGHT_EXE=C:\Program Files\NVIDIA Corporation\Nsight Systems 2026.5.1\target-windows-x64\nsys.exe"
if not exist "%NSIGHT_EXE%" (
    echo Nsight Systems 2026.5.1 was not found. Use gameplay-diagnostics.py --nsight with its actual installed path.
    pause
    exit /b 1
)
where py >nul 2>nul
if errorlevel 1 (
    echo Python launcher was not found. Use Python 3.11 or newer with gameplay-diagnostics.py.
    pause
    exit /b 1
)
py -3 "%~dp0gameplay-diagnostics.py" launch --confirm --nsight "%NSIGHT_EXE%" --profile-cpu --profile-seconds 20 %*
set "RESULT=%ERRORLEVEL%"
pause
exit /b %RESULT%
