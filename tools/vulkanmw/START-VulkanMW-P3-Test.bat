@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title VulkanMW P3 GPU cull + indirect benchmark
echo Four matched runs: P2 control, P3 GPU-cull candidate, candidate, control.
echo Both arms use the complete repaired P1 + P2 stack.
echo Only OPENMW_VK_GPU_CULL_INDIRECT differs between arms.
echo.
call "%~dp0START-VulkanMW-Producer-Test.bat" --gpu-cull-indirect %*
exit /b %ERRORLEVEL%
