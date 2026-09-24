param(
    [Parameter(Mandatory=$true)][string]$RuntimeBase,
    [Parameter(Mandatory=$true)][string]$Destination,
    [string]$BuildLog = 'persistent-architecture-build.log',
    [switch]$Incremental,
    [switch]$Exterior,
    [switch]$Architecture,
    [switch]$Retained
)
$ErrorActionPreference = 'Stop'
$sourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$runtimeRoot = (Resolve-Path -LiteralPath $RuntimeBase).Path
$candidateRoot = [System.IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $candidateRoot) { throw 'Destination must be new; no existing package is overwritten.' }
$engineRoot = Join-Path $sourceRoot 'build/cp4f-engine-vulkan'
$engineExe = Join-Path $engineRoot 'Release/openmw.exe'
if (!(Test-Path -LiteralPath $engineExe)) { throw 'Production Vulkan executable is missing.' }
# Fail before creating/copying a package when a build still holds its log open.
# Pin the input identities and verify them again before publishing the manifest.
$buildLogPath = Join-Path $engineRoot $BuildLog
$buildLogHash = (Get-FileHash -LiteralPath $buildLogPath).Hash.ToLowerInvariant()
$engineHash = (Get-FileHash -LiteralPath $engineExe).Hash.ToLowerInvariant()
$buildCacheHash = (Get-FileHash -LiteralPath (Join-Path $engineRoot 'CMakeCache.txt')).Hash.ToLowerInvariant()
$python = (Get-Command py -ErrorAction Stop).Source
& $python -3 (Join-Path $PSScriptRoot 'shader_resources.py') verify (Join-Path $runtimeRoot 'resources/shaders')
if ($LASTEXITCODE) { throw 'Runtime shader verification failed.' }
New-Item -ItemType Directory -Path $candidateRoot | Out-Null
# Reuse only runtime dependencies/resources. No old executable, diagnostics,
# saves, cache, identity manifest, or test capture is copied.
Get-ChildItem -LiteralPath $runtimeRoot -File -Filter '*.dll' |
    Copy-Item -Destination $candidateRoot
foreach ($name in @('defaults.bin','defaults-cs.bin','gamecontrollerdb.txt','openmw.cfg','openmw.cfg.install')) {
    $inputPath = Join-Path $runtimeRoot $name
    if (Test-Path -LiteralPath $inputPath) { Copy-Item -LiteralPath $inputPath -Destination $candidateRoot }
}
Get-ChildItem -LiteralPath $runtimeRoot -Directory |
    Where-Object { $_.Name -eq 'resources' -or $_.Name -like 'osgPlugins-*' } |
    Copy-Item -Destination $candidateRoot -Recurse
foreach ($name in @('openmw.exe','openmw.pdb','openmw.map')) {
    $inputPath = Join-Path $engineRoot "Release/$name"
    if (Test-Path -LiteralPath $inputPath) { Copy-Item -LiteralPath $inputPath -Destination $candidateRoot }
}
foreach ($name in @('gameplay-diagnostics.py','diagnosticconfig.py','runtime-diagnostics.py','shader_resources.py',
    'Start-Persistent-Vulkan.cmd','Start-Persistent-Control.cmd','Start-Persistent-Profile.cmd','PERSISTENT-ARCHITECTURE-REPAIR.md')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $candidateRoot
}
if ($Incremental) {
    foreach ($name in @('Start-Incremental-Vulkan.cmd','Start-Incremental-Control.cmd','Start-Incremental-Profile.cmd',
        'INCREMENTAL-ARCHITECTURE-REPAIR.md')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $candidateRoot
    }
}
if ($Exterior) {
    foreach ($name in @('Start-Exterior-Repairs.cmd','Start-Exterior-Control.cmd','Start-Exterior-Profile.cmd',
        'EXTERIOR-REPAIR-BATCH.md')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $candidateRoot
    }
}
if ($Architecture) {
    foreach ($name in @('Start-Architecture-Repairs.cmd','Start-Architecture-Control.cmd',
        'INTEGRATED-ARCHITECTURE-REPAIR.md')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $candidateRoot
    }
}
if ($Retained) {
    foreach ($name in @('Start-Retained-Vulkan.cmd','Start-Retained-Control.cmd',
        'END-TO-END-PERSISTENT-RENDERER.md')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $candidateRoot
    }
}
$fileHashes = [ordered]@{}
Push-Location $sourceRoot
try {
    $paths = @(& git ls-files -m -o --exclude-standard) | Sort-Object -Unique
    foreach ($path in $paths) {
        if ($path.EndsWith('.md')) { continue }
        $inputPath = Join-Path $sourceRoot $path
        if (Test-Path -LiteralPath $inputPath -PathType Leaf) {
            $fileHashes[$path] = (Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $candidateHash = (Get-FileHash -LiteralPath (Join-Path $candidateRoot 'openmw.exe')).Hash.ToLowerInvariant()
    if ($candidateHash -ne $engineHash -or
        (Get-FileHash -LiteralPath $engineExe).Hash.ToLowerInvariant() -ne $engineHash -or
        (Get-FileHash -LiteralPath $buildLogPath).Hash.ToLowerInvariant() -ne $buildLogHash) {
        throw 'Build changed during staging; package is incomplete and must not be used.'
    }
    $manifest = [ordered]@{
        schema = 1; kind = $(if ($Retained) { 'retained-renderer-uncommitted-candidate' } elseif ($Architecture) { 'integrated-architecture-uncommitted-candidate' } elseif ($Exterior) { 'exterior-repair-uncommitted-candidate' } elseif ($Incremental) { 'incremental-architecture-uncommitted-candidate' } else { 'persistent-architecture-uncommitted-candidate' })
        source_root = $sourceRoot; branch = (& git branch --show-current); base_commit = (& git rev-parse HEAD)
        changed_source_files = $fileHashes
        executable_sha256 = $candidateHash
        build_log_sha256 = $buildLogHash
        build_cache_sha256 = $buildCacheHash
        runtime_base = $runtimeRoot; created_utc = [DateTime]::UtcNow.ToString('o')
        runtime_performance_accepted = $false; regular_saves_copied = $false
    }
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $candidateRoot 'persistent-build-manifest.json') -Encoding utf8
}
finally { Pop-Location }
& $python -3 (Join-Path $candidateRoot 'shader_resources.py') verify (Join-Path $candidateRoot 'resources/shaders')
if ($LASTEXITCODE) { throw 'Candidate shader verification failed.' }
& (Join-Path $candidateRoot 'openmw.exe') --version
if ($LASTEXITCODE) { throw 'Candidate executable startup/version smoke failed.' }
Get-ChildItem -LiteralPath $candidateRoot -Recurse -File | Measure-Object Length -Sum
Write-Output "Candidate: $candidateRoot"
