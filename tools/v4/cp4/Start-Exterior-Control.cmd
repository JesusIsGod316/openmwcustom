@echo off
setlocal
rem Same NEW executable; disable only exterior-batch mechanisms.
call "%~dp0Start-Persistent-Vulkan.cmd" --cpu-fastpaths incremental %*
exit /b %ERRORLEVEL%
