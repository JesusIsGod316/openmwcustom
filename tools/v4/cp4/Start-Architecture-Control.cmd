@echo off
setlocal
rem Same executable; preserve the previous exterior batch, disable this pass.
call "%~dp0Start-Persistent-Vulkan.cmd" --cpu-fastpaths exterior %*
exit /b %ERRORLEVEL%
