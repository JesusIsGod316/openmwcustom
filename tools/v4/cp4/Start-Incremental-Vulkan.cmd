@echo off
setlocal
rem New architecture controls plus the retained persistent/visibility mechanisms.
call "%~dp0Start-Persistent-Vulkan.cmd" %*
exit /b %ERRORLEVEL%
