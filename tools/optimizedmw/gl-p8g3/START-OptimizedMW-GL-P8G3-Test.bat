@echo off
setlocal
cd /d "%~dp0"
rem Fail before settings changes if an old/mixed shader package was extracted here.
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { $r=Join-Path (Get-Location) 'resources\shaders'; $m=Get-Content -Raw -LiteralPath (Join-Path $r 'shader-package.json') | ConvertFrom-Json; if($m.groundcover_patch.id -ne 'optimizedmw-p8g3-pbr-groundcover'){throw 'Missing P8G3 PBR shader deployment. Extract the complete build into a new folder.'}; foreach($n in @('compatibility/groundcover.vert','compatibility/groundcover.frag','compatibility/groundcover_lod.glsl')){ $expected=$m.files.PSObject.Properties[$n].Value; $actual=(Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $r $n)).Hash; if(-not $expected -or $actual -ne $expected){throw ('Changed P8G3 shader: '+$n)} }; Write-Host 'P8G3 deployed shader hashes verified.' -ForegroundColor Green } catch { Write-Host $_ -ForegroundColor Red; exit 1 }"
if errorlevel 1 (
    pause
    exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0OptimizedMW_GL-P8G3_Test.ps1"
endlocal
