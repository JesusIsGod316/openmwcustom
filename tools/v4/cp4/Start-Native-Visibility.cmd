@echo off
setlocal
set "OPENMW_V4_STATIC_FRUSTUM=1"
set "OPENMW_V4_TERRAIN_OCCLUSION=1"
set "OPENMW_V4_POSTPROCESS="
py -3 "%~dp0gameplay-diagnostics.py" launch --confirm --cpu-fastpaths all %*
set "RESULT=%ERRORLEVEL%"
pause
exit /b %RESULT%
