@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title VulkanMW isolated gameplay benchmark
echo VulkanMW gameplay benchmark using a private copy of your settings and saves.
echo All game writes remain in UserProfile. Normal mod directories are referenced read-only.
echo Stage 1 object binding updates and retained populations are enabled.
echo Use --binding-control to disable only binding updates, or --p3b-control for the older P3B path.
echo Load a save or start a game, test normally, then quit normally to finish the log.
echo Logs are saved under this package's Benchmarks directory.
echo.
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
if "%PYMODE%"=="py" (
    py -3 "%~dp0tools\vulkanmw\vulkan-play.py" --package "%~dp0." --object-binding-updates --benchmark %*
) else (
    python "%~dp0tools\vulkanmw\vulkan-play.py" --package "%~dp0." --object-binding-updates --benchmark %*
)
set "RESULT=%ERRORLEVEL%"
if not "%RESULT%"=="0" echo VulkanMW exited with code %RESULT%. Inspect the latest Benchmarks session.
pause
exit /b %RESULT%
