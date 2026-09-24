@echo off
setlocal
rem Same NEW executable, retaining the previous persistent optimizations.
rem Disables only this batch's incremental capture/population controls.
call "%~dp0Start-Persistent-Vulkan.cmd" --cpu-fastpaths persistent %*
exit /b %ERRORLEVEL%
