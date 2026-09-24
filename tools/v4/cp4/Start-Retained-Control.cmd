@echo off
setlocal
rem Same executable and previous producer path; disables the four retained mechanisms.
call "%~dp0Start-Persistent-Vulkan.cmd" --cpu-fastpaths previous-retained %*
exit /b %ERRORLEVEL%
