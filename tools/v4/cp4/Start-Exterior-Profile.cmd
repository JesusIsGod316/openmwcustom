@echo off
setlocal
rem Optional Nsight capture. Requires an administrator terminal, no auto-elevation.
call "%~dp0Start-Persistent-Profile.cmd" %*
exit /b %ERRORLEVEL%
