@echo off
setlocal
rem Disable only the three new CPU optimizations, preserving other repairs.
py -3 "%~dp0gameplay-diagnostics.py" launch --confirm --cpu-fastpaths control %*
set "RESULT=%ERRORLEVEL%"
pause
exit /b %RESULT%
