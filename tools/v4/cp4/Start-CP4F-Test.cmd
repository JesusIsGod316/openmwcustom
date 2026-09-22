@echo off
setlocal
cd /d "%~dp0"
py -3 -c "import sys; sys.exit(0 if sys.version_info >= (3,11) else 1)" >nul 2>&1
if not errorlevel 1 (
    py -3 "%~dp0gameplay-diagnostics.py" launch --confirm %*
    goto finish
)
python -c "import sys; sys.exit(0 if sys.version_info >= (3,11) else 1)" >nul 2>&1
if errorlevel 1 (
    echo Python 3.11 or newer was not found. No game has been launched.
    pause
    exit /b 1
)
python "%~dp0gameplay-diagnostics.py" launch --confirm %*
:finish
set "result=%errorlevel%"
echo.
echo Diagnostic helper finished with exit code %result%.
pause
exit /b %result%
