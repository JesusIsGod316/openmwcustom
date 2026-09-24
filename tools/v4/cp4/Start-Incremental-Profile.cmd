@echo off
setlocal
rem Optional Nsight route; requires the user's explicit administrator launch.
call "%~dp0Start-Persistent-Profile.cmd" %*
exit /b %ERRORLEVEL%
