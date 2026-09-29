@echo off
setlocal
cd /d "%~dp0"
rem Fail before settings changes if an old/mixed shader package was extracted here.
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { $r=Join-Path (Get-Location) 'resources\shaders'; $m=Get-Content -Raw -LiteralPath (Join-Path $r 'shader-package.json') | ConvertFrom-Json; if($m.groundcover_patch.id -ne 'optimizedmw-p8g4-pbr-groundcover'){throw 'Missing P8G4 PBR shader deployment. Extract the complete build into a new folder.'}; foreach($n in @('compatibility/groundcover.vert','compatibility/groundcover.frag','compatibility/groundcover_lod.glsl','compatibility/temporal_camera_motion.vert','compatibility/temporal_camera_motion.frag','compatibility/temporal_motion_view.vert','compatibility/temporal_motion_view.frag')){ $expected=$m.files.PSObject.Properties[$n].Value; $actual=(Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $r $n)).Hash; if(-not $expected -or $actual -ne $expected){throw ('Changed shader: '+$n)} }; Write-Host 'Phase 9 deployed shader hashes verified.' -ForegroundColor Green } catch { Write-Host $_ -ForegroundColor Red; exit 1 }"
if errorlevel 1 (
    pause
    exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0OptimizedMW_Test.ps1"
if errorlevel 1 (
    echo Phase 9 launcher reported an error. Raw profiles are retained in Documents\My Games\OpenMW.
    pause
    exit /b 1
)
endlocal
