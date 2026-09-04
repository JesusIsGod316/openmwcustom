@echo off
setlocal

rem OpenMW Custom Build V4 CP0 visual-corpus compatibility variant.
rem This preserves the final V3.25 Mode151 cadence-2 control and changes only
rem the V3.21 full-body-first-person feature gate to OFF so native first person
rem has an explicit renderer-parity reference.
rem
rem Usage:
rem   V4-CP0-NativeFP-Reference.bat "D:\path\to\final-v3.25-build"
rem If no argument is supplied, the current directory is treated as the build root.

set "BUILDROOT=%~1"
if "%BUILDROOT%"=="" set "BUILDROOT=%CD%"

if not exist "%BUILDROOT%\openmw-launcher.exe" (
    echo ERROR: openmw-launcher.exe not found in "%BUILDROOT%".
    exit /b 1
)

rem Keep rejected/experimental later-V3 scheduling paths OFF.
set "OPENMW_V320_FOCUS_ADAPTIVE=0"
set "OPENMW_V321_COMPLETION_GOVERNOR=0"
set "OPENMW_V322_CP1_MSOC_HOT_PATH=0"
set "OPENMW_V322_CP2_OCCLUDER_EFFICIENCY_MODE=0"
set "OPENMW_V322_PARALLEL_ACTOR_AVOIDANCE=0"
set "OPENMW_V323_PARALLEL_MSOC_MODE=0"
set "OPENMW_V324_ASYNC_MSOC=0"

rem Preserve the accepted final Mode151 stack.
set "OPENMW_V319_FOCUS_CADENCE=2"
set "OPENMW_V320_ENGINE_LUA_FASTPATHS=1"
set "OPENMW_V320_SOUND_CONVERSION_CACHE=1"
set "OPENMW_V320_SOUND_QUERY_COALESCING=1"
set "OPENMW_V320_LUA_PROFILER_CAPABLE=1"
set "OPENMW_V321_CP2_FAIRNESS=1"
set "OPENMW_V321_CP4_SHADOW_COMPAT=1"
set "OPENMW_V324_FRAME_JOB_QOS=1"
set "OPENMW_V325_ACTOR_SOURCE_BATCH=1"
set "OPENMW_V325_PARALLEL_ACTOR_BINDING=1"

rem The one deliberate compatibility delta from the frozen gaming wrapper.
rem camera.cpp treats this process environment value as an override of the
rem configured [Camera] v3.21 full body first person setting.
set "OPENMW_V321_CP3_FULL_BODY_FIRST_PERSON=0"

rem Keep diagnostics and explicit OSG threading overrides off.
set "OPENMW_V324_DEEP_TELEMETRY=0"
set "OPENMW_V324_DEEP_FILE="
set "OPENMW_V325_JOBGROUP_STATS_FILE="
set "OPENMW_V317_LUA_OPT="
set "OSG_THREADING="

start "" "%BUILDROOT%\openmw-launcher.exe"
endlocal
