@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title VulkanMW P2 persistent GPU table benchmark
echo Four matched runs: repaired P1 control, P2 GPU-table candidate, candidate, control.
echo Only OPENMW_VK_GPU_SCENE_TABLES differs between arms.
echo.
call "%~dp0START-VulkanMW-Producer-Test.bat" --gpu-scene-tables %*
exit /b %ERRORLEVEL%
