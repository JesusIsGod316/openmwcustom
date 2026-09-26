@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title VulkanMW Automated Test / Benchmark

rem Permanent VulkanMW benchmark launcher.
rem Profile matches the Phase 2 automated comparison:
rem Vulkan/VSG, retained fast paths, 1920x1080, uncapped,
rem Seyda Neen seed 123456, 15 s warmup + 30 s measured.
rem The Python driver creates an isolated private test profile and never edits
rem the normal OpenMW configuration or saves.

if not exist "%~dp0openmw.exe" (
    echo ERROR: openmw.exe is missing from this VulkanMW package.
    pause
    exit /b 1
)
if not exist "%~dp0architecture-benchmark.py" (
    echo ERROR: architecture-benchmark.py is missing from this VulkanMW package.
    pause
    exit /b 1
)
if not exist "%~dp0architecture-benchmark.lua" (
    echo ERROR: architecture-benchmark.lua is missing from this VulkanMW package.
    pause
    exit /b 1
)
if not exist "%~dp0gameplay-diagnostics.py" (
    echo ERROR: gameplay-diagnostics.py is missing from this VulkanMW package.
    pause
    exit /b 1
)
if not exist "%~dp0diagnosticconfig.py" (
    echo ERROR: diagnosticconfig.py is missing from this VulkanMW package.
    pause
    exit /b 1
)

set "PYMODE="
py -3 -c "import sys; sys.exit(0 if sys.version_info >= (3,11) else 1)" >nul 2>&1
if not errorlevel 1 set "PYMODE=py"
if not defined PYMODE (
    python -c "import sys; sys.exit(0 if sys.version_info >= (3,11) else 1)" >nul 2>&1
    if not errorlevel 1 set "PYMODE=python"
)
if not defined PYMODE (
    echo ERROR: Python 3.11 or newer was not found.
    echo Install Python 3.11+ and run this launcher again.
    pause
    exit /b 1
)

for /f %%I in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "STAMP=%%I"
set "OUT=%~dp0VulkanMW-Benchmark-%STAMP%"

echo.
echo ============================================================
echo VulkanMW automated benchmark
echo ============================================================
echo Renderer: Vulkan/VSG
echo Fast-path profile: retained
echo Resolution: 1920x1080
echo VSync / frame cap: disabled by benchmark driver
echo Scene: Seyda Neen
echo Random seed: 123456
echo Warmup: 15 seconds
echo Measurement: 30 seconds
echo Output: "%OUT%"
echo.
echo The benchmark launches automatically and exits automatically.
echo Do not launch another OpenMW instance during the run.
echo.

if "%PYMODE%"=="py" (
    py -3 "%~dp0architecture-benchmark.py" --executable "%~dp0openmw.exe" --output "%OUT%" --renderer vulkan --fastpaths retained %*
) else (
    python "%~dp0architecture-benchmark.py" --executable "%~dp0openmw.exe" --output "%OUT%" --renderer vulkan --fastpaths retained %*
)
set "RESULT=%ERRORLEVEL%"

if not "%RESULT%"=="0" (
    echo.
    echo Benchmark FAILED with exit code %RESULT%.
    echo Evidence remains in:
    echo "%OUT%"
    pause
    exit /b %RESULT%
)

echo.
echo Creating shareable benchmark ZIP...
powershell -NoProfile -Command "Compress-Archive -LiteralPath '%OUT%' -DestinationPath '%OUT%.zip' -CompressionLevel Optimal -Force"
if errorlevel 1 (
    echo WARNING: Benchmark passed, but ZIP creation failed.
    echo Results remain in:
    echo "%OUT%"
    pause
    exit /b 0
)

echo.
echo Benchmark complete.
echo Upload this file to ChatGPT:
echo "%OUT%.zip"
echo.
pause
exit /b 0
