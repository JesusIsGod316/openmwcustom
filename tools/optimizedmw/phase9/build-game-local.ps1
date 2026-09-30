param(
    [string]$BuildDirectory = "$env:TEMP\optimizedmw-phase9-audited-game",
    [string]$DependencyDirectory = "$env:TEMP\openmw-fullbody-combat-deps\vcpkg",
    [ValidateSet('Configure', 'Build', 'Test', 'Install')][string]$Stage = 'Configure',
    [string]$InstallDirectory = "$env:TEMP\OptimizedMW-Phase9-Audited",
    [int]$Jobs = 6
)
$ErrorActionPreference = 'Stop'
$sourceDirectory = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$visualStudio = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
if (-not $env:VSINSTALLDIR -or $env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or
    $env:VSINSTALLDIR.TrimEnd('\') -ne $visualStudio.TrimEnd('\')) {
    Import-Module (Join-Path $visualStudio 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $visualStudio -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}
$cmakeDirectory = Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$cmake = Join-Path $cmakeDirectory 'cmake.exe'
$ninja = Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$installed = Join-Path $DependencyDirectory 'installed\x64-windows'
foreach ($runtimeDirectory in @("$installed\bin\Release", "$installed\bin")) {
    if (($env:PATH -split ';') -notcontains $runtimeDirectory) { $env:PATH = "$runtimeDirectory;$env:PATH" }
}
if (Test-Path -LiteralPath (Join-Path $BuildDirectory '_deps')) {
    foreach ($sdl in (Get-ChildItem -LiteralPath (Join-Path $BuildDirectory '_deps') -Directory -Filter 'SDL3-*')) {
        $runtimeDirectory = Join-Path $sdl.FullName 'lib\x64'
        if (($env:PATH -split ';') -notcontains $runtimeDirectory) { $env:PATH = "$runtimeDirectory;$env:PATH" }
    }
}
$env:PKG_CONFIG_PATH = "$installed\lib\pkgconfig;$installed\share\pkgconfig"
if ($Stage -eq 'Configure') {
    $arguments = @(
        '-S', $sourceDirectory, '-B', $BuildDirectory, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=RelWithDebInfo', "-DCMAKE_MAKE_PROGRAM=$ninja",
        # Prefer release deps; the empty fallback permits unconfigured imports
        # such as the upstream SDL3 Windows package.
        '-DCMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO=Release;',
        '-DCMAKE_TOOLCHAIN_FILE=', "-DCMAKE_PREFIX_PATH=$installed",
        "-DPKG_CONFIG_EXECUTABLE=$installed\tools\pkgconf\pkgconf.exe",
        "-DBULLET_INCLUDE_DIR=$installed\include\bullet",
        "-DLuaJit_INCLUDE_DIR=$installed\include\luajit", "-DLuaJit_LIBRARY=$installed\lib\lua51.lib",
        '-DBUILD_OPENMW=ON', '-DBUILD_OPENMW_TESTS=ON', '-DBUILD_COMPONENTS_TESTS=ON',
        '-DBUILD_OPENCS=OFF', '-DBUILD_OPENCS_TESTS=OFF', '-DBUILD_LAUNCHER=OFF', '-DBUILD_WIZARD=OFF',
        '-DBUILD_BENCHMARKS=OFF', '-DBUILD_BSATOOL=OFF', '-DBUILD_ESMTOOL=OFF',
        '-DBUILD_ESSIMPORTER=OFF', '-DBUILD_MWINIIMPORTER=OFF', '-DBUILD_NAVMESHTOOL=OFF',
        '-DBUILD_NIFTEST=OFF', '-DBUILD_BULLETOBJECTTOOL=OFF', '-DBUILD_DOCS=OFF',
        '-DOPENMW_USE_SYSTEM_SQLITE3=OFF', '-DOPENMW_USE_SYSTEM_YAML_CPP=OFF',
        '-DOPENMW_ENABLE_V4_VULKAN_RUNTIME=OFF', '-DOPENMW_LTO_BUILD=OFF',
        '-DOPENMW_PHASE9_BUILD_NATIVE_GAME_TESTS=ON',
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'
    )
    & $cmake @arguments
} elseif ($Stage -eq 'Build') {
    & $cmake --build $BuildDirectory --parallel $Jobs
} elseif ($Stage -eq 'Test') {
    # Upstream's two GTest executables are not registered with CTest.
    foreach ($suite in @('openmw-tests', 'components-tests')) {
        & (Join-Path $BuildDirectory "$suite.exe") "--gtest_output=xml:$BuildDirectory/$suite-results.xml"
        if ($LASTEXITCODE -ne 0) { throw "$suite failed with exit code $LASTEXITCODE" }
    }
    & (Join-Path $cmakeDirectory 'ctest.exe') --test-dir $BuildDirectory -R '^p9-owned-postpasses$' --output-on-failure --timeout 45
} else {
    $revision = (& git -C $sourceDirectory rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot identify the game source revision' }
    $changes = & git -C $sourceDirectory status --porcelain
    if ($LASTEXITCODE -ne 0 -or $changes) { throw 'Commit the final source before installing a reproducible candidate' }
    $versionSource = Join-Path $BuildDirectory 'components\version\version.cpp'
    if (-not (Test-Path -LiteralPath $versionSource) -or
        -not (Get-Content -Raw -LiteralPath $versionSource).Contains($revision)) {
        throw 'Reconfigure and build the game at the final commit before installation'
    }
    & $cmake --install $BuildDirectory --prefix $InstallDirectory --config RelWithDebInfo
    if ($LASTEXITCODE -ne 0) { throw "Runtime installation failed with exit code $LASTEXITCODE" }
    # Prefix-only discovery deliberately avoids vcpkg's app-local deployment.
    # Copy the matching pinned release DLLs and OSG image plugins explicitly.
    Get-ChildItem -LiteralPath (Join-Path $installed 'bin') -Filter '*.dll' |
        Copy-Item -Destination $InstallDirectory
    Get-ChildItem -LiteralPath (Join-Path $installed 'bin\Release') -Filter '*.dll' |
        Copy-Item -Destination $InstallDirectory
    foreach ($pluginRoot in @((Join-Path $installed 'plugins'), (Join-Path $installed 'bin'))) {
        if (Test-Path -LiteralPath $pluginRoot) {
            foreach ($plugins in (Get-ChildItem -LiteralPath $pluginRoot -Directory -Filter 'osgPlugins-*')) {
                $destination = Join-Path $InstallDirectory $plugins.Name
                New-Item -ItemType Directory -Path $destination -Force | Out-Null
                Get-ChildItem -LiteralPath $plugins.FullName |
                    Copy-Item -Destination $destination -Recurse -Force
            }
        }
    }
    @("Ref $(& git -C $sourceDirectory branch --show-current)", 'Job local Windows MSVC build', "Commit $revision") |
        Set-Content -LiteralPath (Join-Path $InstallDirectory 'CI-ID.txt') -Encoding ascii
}
if ($LASTEXITCODE -ne 0) { throw "Phase 9 game $Stage failed with exit code $LASTEXITCODE" }
