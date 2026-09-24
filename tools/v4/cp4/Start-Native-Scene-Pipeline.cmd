@echo off
setlocal
rem Identity scene-color/depth path, not bloom/clouds or the Rafael shader chain.
set "OPENMW_V4_STATIC_FRUSTUM=1"
set "OPENMW_V4_TERRAIN_OCCLUSION=1"
set "OPENMW_V4_POSTPROCESS=copy"
py -3 "%~dp0gameplay-diagnostics.py" launch --confirm --cpu-fastpaths all %*
set "RESULT=%ERRORLEVEL%"
pause
exit /b %RESULT%
