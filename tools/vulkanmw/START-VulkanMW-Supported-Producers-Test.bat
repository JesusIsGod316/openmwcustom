@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title VulkanMW supported actor and particle producer benchmark
echo Four matched runs: queues-only control, supported-producer candidate, candidate, control.
echo Only OPENMW_VK_SUPPORTED_CONTINUOUS_PRODUCERS differs between arms.
echo.
call "%~dp0START-VulkanMW-Producer-Test.bat" --supported-continuous %*
exit /b %ERRORLEVEL%
