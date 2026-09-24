@echo off
setlocal
rem New executable, previous optimizations plus the exterior repair batch.
call "%~dp0Start-Persistent-Vulkan.cmd" %*
exit /b %ERRORLEVEL%
