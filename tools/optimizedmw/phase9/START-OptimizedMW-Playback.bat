@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0OptimizedMW_RoutePlayback.ps1" -GameDir "%~dp0."
if errorlevel 1 echo Playback failed. Preserve its private session and logs for inspection.
pause
endlocal
