param(
    [ValidateSet('Configure','Build','Test')][string]$Stage = 'Configure',
    [string]$BuildDirectory = "$env:TEMP\optimizedmw-phase9-audited-native",
    [string]$DependencyDirectory = "$env:TEMP\openmw-fullbody-combat-deps\vcpkg",
    [string]$SdlDirectory = "$env:TEMP\openmw-fullbody-combat-native-build\_deps\SDL3-3.4.10",
    [switch]$Interop,
    [int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
$visualStudio = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
Import-Module (Join-Path $visualStudio 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $visualStudio -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$cmake = Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
$installed = Join-Path $DependencyDirectory 'installed\x64-windows'
$env:PATH = "$installed\bin;$installed\bin\Release;$(Join-Path $SdlDirectory 'lib\x64');$env:PATH"
if ($Stage -eq 'Configure') {
    foreach ($required in @($cmake,$ninja,(Join-Path $installed 'include\osg\Version'),(Join-Path $SdlDirectory 'include\SDL3\SDL.h'))) {
        if (-not (Test-Path -LiteralPath $required)) { throw "Required native dependency missing: $required" }
    }
    & $cmake -S $PSScriptRoot -B $BuildDirectory -G Ninja `
        '-DCMAKE_BUILD_TYPE=RelWithDebInfo' "-DCMAKE_MAKE_PROGRAM=$ninja" `
        "-DCMAKE_PREFIX_PATH=$installed" '-DCMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO=Release;' `
        '-DP9_REQUIRE_GAME_SDK=ON' "-DP9_SDL3_INCLUDE=$(Join-Path $SdlDirectory 'include')" `
        "-DP9_MYGUI_INCLUDE=$(Join-Path $installed 'include\MYGUI')" `
        "-DP9_BULLET_INCLUDE=$(Join-Path $installed 'include\bullet')" `
        "-DP9_YAML_INCLUDE=$env:TEMP\openmw-fullbody-combat-native-build\extern\fetched\yaml-cpp\include" `
        "-DP9_DETOUR_INCLUDE=$env:TEMP\optimizedmw-phase9-audited-game\extern\fetched\recastnavigation\Detour\Include" `
        "-DP9_RECAST_INCLUDE=$env:TEMP\optimizedmw-phase9-audited-game\extern\fetched\recastnavigation\Recast\Include" `
        "-DOPENMW_PHASE9_INTEROP_FIXTURE=$($Interop.IsPresent.ToString().ToUpperInvariant())"
} elseif ($Stage -eq 'Build') {
    & $cmake --build $BuildDirectory --parallel $Jobs
} else {
    & $ctest --test-dir $BuildDirectory --output-on-failure --timeout 45
}
if ($LASTEXITCODE -ne 0) { throw "Phase 9 $Stage failed with exit code $LASTEXITCODE" }
