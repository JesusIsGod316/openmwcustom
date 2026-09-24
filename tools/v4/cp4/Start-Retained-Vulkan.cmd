@echo off
setlocal
rem Measured retained profile: excludes the separate batch texture metadata experiment.
call "%~dp0Start-Persistent-Vulkan.cmd" --cpu-fastpaths retained %*
exit /b %ERRORLEVEL%
