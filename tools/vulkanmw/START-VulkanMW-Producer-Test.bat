@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title VulkanMW producer queue benchmark
if not exist "%~dp0openmw.exe" (
    echo ERROR: Extract the complete package before running the benchmark.
    pause
    exit /b 1
)
set "PYMODE="
py -3 -c "import sys; sys.exit(0 if sys.version_info >= (3,11) else 1)" >nul 2>&1
if not errorlevel 1 set "PYMODE=py"
if not defined PYMODE (
    python -c "import sys; sys.exit(0 if sys.version_info >= (3,11) else 1)" >nul 2>&1
    if not errorlevel 1 set "PYMODE=python"
)
if not defined PYMODE (
    echo ERROR: Python 3.11 or newer is required.
    pause
    exit /b 1
)
echo Four matched runs: control, candidate, candidate, control.
echo Each uses a private new-game profile, 15s warmup and 30s measurement.
echo Existing content is preserved. Normal settings and saves are not modified.
echo Results are zipped under the Benchmarks folder, including failed-run evidence.
echo.
if "%PYMODE%"=="py" (
    py -3 "%~dp0tools\vulkanmw\run-producer-queues.py" --package "%~dp0." %*
) else (
    python "%~dp0tools\vulkanmw\run-producer-queues.py" --package "%~dp0." %*
)
set "RESULT=%ERRORLEVEL%"
if not "%RESULT%"=="0" echo ERROR: Benchmark incomplete. Keep the generated evidence ZIP.
pause
exit /b %RESULT%
