@echo off
setlocal
rem Reuse the ordinary shader-package check and numeric manual launcher.
rem No automated route, timed input, or game launch is added here.
call "%~dp0START-OptimizedMW-Test.bat" -FrozenConfigDirectory "%~dp0Configuration\Benchmark-SaintJs" %*
exit /b %errorlevel%
