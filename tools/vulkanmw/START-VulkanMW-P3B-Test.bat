@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title VulkanMW P3B compacted indirect benchmark
echo Four matched runs: P3A control, P3B compacted candidate, candidate, control.
echo Both arms use repaired P1 + P2 + P3A GPU cull/indirect.
echo Only OPENMW_VK_GPU_CULL_COMPACT differs between arms.
echo.
call "%~dp0START-VulkanMW-Producer-Test.bat" --gpu-cull-compact %*
exit /b %ERRORLEVEL%
