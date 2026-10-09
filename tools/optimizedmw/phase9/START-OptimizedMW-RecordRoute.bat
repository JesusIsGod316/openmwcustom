@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0OptimizedMW_RouteRecorder.ps1" %*
if errorlevel 1 echo Route recording failed. Preserve the private session and logs for inspection.
pause
