@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title VulkanMW normal-play QA
echo VulkanMW normal-play QA: normal mods, settings and saves remain in use.
echo Renderer is forced to Vulkan only for this process; normal settings files are not edited.
echo Current repaired P1 plus P2 GPU tables are enabled. Add --p1-only for the P1 control.
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
    py -3 "%~dp0tools\vulkanmw\vulkan-play.py" --package "%~dp0." %*
) else (
    python "%~dp0tools\vulkanmw\vulkan-play.py" --package "%~dp0." %*
)
set "RESULT=%ERRORLEVEL%"
if not "%RESULT%"=="0" echo VulkanMW exited with code %RESULT%.
pause
exit /b %RESULT%
