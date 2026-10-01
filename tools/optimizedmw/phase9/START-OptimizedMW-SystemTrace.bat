@echo off
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0OptimizedMW_SystemTrace.ps1" %*
echo.
pause
