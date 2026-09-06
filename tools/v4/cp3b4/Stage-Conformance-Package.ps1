param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [Parameter(Mandatory = $true)]
    [string[]]$RuntimeBin,

    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Resolve-FullPath([string]$Path) {
    return [System.IO.Path]::GetFullPath($Path)
}

function Copy-UniqueFile([string]$Source, [string]$DestinationDirectory) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Package source file does not exist: $Source"
    }

    $destination = Join-Path $DestinationDirectory ([System.IO.Path]::GetFileName($Source))
    if (Test-Path -LiteralPath $destination -PathType Leaf) {
        $sourceHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
        $destinationHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
        if ($sourceHash -ne $destinationHash) {
            throw "Runtime package collision has different content: $destination"
        }
        return
    }

    Copy-Item -LiteralPath $Source -Destination $destination
}

$exe = Resolve-FullPath $Executable
$repo = Resolve-FullPath $RepositoryRoot
$out = Resolve-FullPath $OutputDirectory

if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "Conformance executable does not exist: $exe"
}
if (-not (Test-Path -LiteralPath $repo -PathType Container)) {
    throw "Repository root does not exist: $repo"
}

if (Test-Path -LiteralPath $out) {
    Remove-Item -LiteralPath $out -Recurse -Force
}
New-Item -ItemType Directory -Path $out -Force | Out-Null

Copy-UniqueFile $exe $out

$resolvedBins = @()
foreach ($bin in $RuntimeBin) {
    $resolved = Resolve-FullPath $bin
    if (-not (Test-Path -LiteralPath $resolved -PathType Container)) {
        throw "Runtime bin directory does not exist: $resolved"
    }
    $resolvedBins += $resolved
}

# The conformance package is a diagnostic artifact, not a production installer.
# Stage all runtime DLLs from the exact dependency roots used to link this tool.
# This is intentionally more conservative than maintaining a hand-written DLL
# allowlist that can silently become stale as VSG/OpenMW dependencies evolve.
foreach ($bin in $resolvedBins) {
    Get-ChildItem -LiteralPath $bin -Filter '*.dll' -File | Sort-Object Name | ForEach-Object {
        Copy-UniqueFile $_.FullName $out
    }
}

$toolDirectory = Join-Path $out 'cp3b4'
New-Item -ItemType Directory -Path $toolDirectory -Force | Out-Null
$cp3b4Source = Join-Path $repo 'tools/v4/cp3b4'
foreach ($name in @(
    'corpus-runner.py',
    'corpus.schema.json',
    'corpus.local.example.json',
    'README.md'
)) {
    Copy-Item -LiteralPath (Join-Path $cp3b4Source $name) -Destination (Join-Path $toolDirectory $name)
}

$launcher = @'
param(
    [Parameter(Mandatory = $true)] [string]$Manifest,
    [Parameter(Mandatory = $true)] [string[]]$Data,
    [string[]]$Archive = @(),
    [string]$Output = '.\cp3b4-corpus-report.json',
    [int]$DeterminismRuns = 2
)

$ErrorActionPreference = 'Stop'
$packageRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$arguments = @(
    (Join-Path $packageRoot 'cp3b4\corpus-runner.py'),
    '--tool', (Join-Path $packageRoot 'openmw-vulkan-nif-conformance.exe'),
    '--manifest', $Manifest,
    '--output', $Output,
    '--determinism-runs', [string]$DeterminismRuns
)
foreach ($root in $Data) { $arguments += @('--data', $root) }
foreach ($archivePath in $Archive) { $arguments += @('--archive', $archivePath) }
& python @arguments
exit $LASTEXITCODE
'@
Set-Content -LiteralPath (Join-Path $out 'Run-CP3B4-Corpus.ps1') -Value $launcher -Encoding UTF8

$manifestPath = Join-Path $out 'PACKAGE-SHA256.txt'
$packageFiles = Get-ChildItem -LiteralPath $out -Recurse -File |
    Where-Object { $_.FullName -ne $manifestPath } |
    Sort-Object { $_.FullName.Substring($out.Length).Replace('\\', '/') }

$manifestLines = foreach ($file in $packageFiles) {
    $relative = $file.FullName.Substring($out.Length).TrimStart('\\', '/').Replace('\\', '/')
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $relative"
}
Set-Content -LiteralPath $manifestPath -Value $manifestLines -Encoding ASCII

Write-Host "CP3B4 conformance package staged: $out"
Write-Host "Files: $($packageFiles.Count + 1)"
Write-Host "Manifest: $manifestPath"
