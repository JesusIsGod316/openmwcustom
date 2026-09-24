@echo off
setlocal
rem Same executable as the control. No profiler or elevation required.
py -3 "%~dp0gameplay-diagnostics.py" launch --confirm --cpu-fastpaths all %*
set "RESULT=%ERRORLEVEL%"
pause
exit /b %RESULT%
