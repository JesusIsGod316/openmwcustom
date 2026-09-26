@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0OptimizedMW_GL-P6_Test.ps1"
endlocal
