@echo off
setlocal
call "%~dp0Start-Persistent-Vulkan.cmd" %*
exit /b %ERRORLEVEL%
