@echo off
setlocal
rem Same executable and CPU fast paths; only the new visibility/scene paths off.
set "OPENMW_V4_STATIC_FRUSTUM="
set "OPENMW_V4_TERRAIN_OCCLUSION="
set "OPENMW_V4_POSTPROCESS="
py -3 "%~dp0gameplay-diagnostics.py" launch --confirm --cpu-fastpaths all %*
set "RESULT=%ERRORLEVEL%"
pause
exit /b %RESULT%
