param (
    [switch] $SkipCompress
)

$ErrorActionPreference = "Stop"

if (-Not (Test-Path CMakeCache.txt))
{
    Write-Error "This script must be run from the build directory."
}

# V3 artifact identity gate.
# This runs against the ACTUAL compiled openmw.exe in the Windows build job,
# before symbols/install artifacts are uploaded. A V3.x branch must contain its
# own version marker in the binary and generated launcher. This catches exactly
# the class of failure where preflight validates V3.N but Windows compiles V3.N-1.
$branch = $env:GITHUB_REF_NAME
if ($branch -match '^v3\.(\d+)(?:-.+)?$')
{
    $minor = $Matches[1]
    $versionLabel = "V3.$minor"
    $versionMarker = "v3.$minor"
    $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
    $launcherPath = Join-Path $repoRoot 'tools\v3\launchers\V3_Lab.ps1'
    $identityPath = Join-Path $repoRoot 'V3-BUILD-IDENTITY.txt'

    if (-Not (Test-Path '.\openmw.exe'))
    {
        Write-Error "V3 artifact identity failure: compiled openmw.exe is missing."
    }

    # Search the real PE image for an ASCII version key emitted by this version's
    # settings/diagnostic strings. An older engine cannot satisfy a newer marker.
    python -c "import sys; from pathlib import Path; marker=sys.argv[1].encode('ascii'); data=Path('openmw.exe').read_bytes(); sys.exit(0 if marker in data else 9)" $versionMarker
    if ($LASTEXITCODE -ne 0)
    {
        Write-Error "V3 artifact identity failure: compiled openmw.exe contains no '$versionMarker' marker. Refusing to upload a stale-version binary."
    }

    if (-Not (Test-Path $launcherPath))
    {
        Write-Error "V3 artifact identity failure: generated V3_Lab.ps1 is missing."
    }
    $launcherText = Get-Content -Raw $launcherPath
    if (-Not $launcherText.Contains($versionLabel))
    {
        Write-Error "V3 artifact identity failure: generated launcher does not identify $versionLabel."
    }

    # New fail-closed router writes this manifest. Require it on all V3 builds
    # going forward so source routing and compiled-artifact identity are linked.
    if (-Not (Test-Path $identityPath))
    {
        Write-Error "V3 artifact identity failure: V3-BUILD-IDENTITY.txt is missing."
    }
    $identityText = Get-Content -Raw $identityPath
    if (-Not $identityText.Contains("version=$versionLabel"))
    {
        Write-Error "V3 artifact identity failure: build identity manifest does not match $versionLabel."
    }
    if (-Not $identityText.Contains('generated_source_identity=passed'))
    {
        Write-Error "V3 artifact identity failure: generated-source identity gate was not recorded as passed."
    }

    Write-Output "V3 artifact identity passed: $versionLabel source, launcher, and compiled openmw.exe agree."
}

if (-Not (Test-Path .cmake\api\v1\reply\index-*.json) -Or -Not ((Get-Content -Raw .cmake\api\v1\reply\index-*.json | ConvertFrom-Json).reply.PSObject.Properties.Name -contains "codemodel-v2"))
{
    Write-Output "Running CMake query..."
    New-Item -Type File -Force .cmake\api\v1\query\codemodel-v2
    cmake .
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Command exited with code $LASTEXITCODE"
    }
    Write-Output "Done."
}

try
{
    Push-Location .cmake\api\v1\reply

    $index = Get-Content -Raw index-*.json | ConvertFrom-Json

    $codemodel = Get-Content -Raw $index.reply."codemodel-v2".jsonFile | ConvertFrom-Json

    $targets = @()
    $codemodel.configurations | ForEach-Object {
        $_.targets | ForEach-Object {
            $target = Get-Content -Raw $_.jsonFile | ConvertFrom-Json
            if ($target.type -eq "EXECUTABLE" -or $target.type -eq "SHARED_LIBRARY")
            {
                $targets += $target
            }
        }
    }

    $artifacts = @()
    $targets | ForEach-Object {
        $_.artifacts | ForEach-Object {
            $artifacts += $_.path
        }
    }
}
finally
{
    Pop-Location
}

if (-not (Test-Path symstore-venv))
{
    python -m venv symstore-venv
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Command exited with code $LASTEXITCODE"
    }
}
$symstoreVersion = "0.3.4"
if (-not (Test-Path symstore-venv\Scripts\symstore.exe) -or -not ((symstore-venv\Scripts\pip show symstore | Select-String '(?<=Version: ).*').Matches.Value -eq $symstoreVersion))
{
    symstore-venv\Scripts\pip install symstore==$symstoreVersion
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Command exited with code $LASTEXITCODE"
    }
}

$artifacts = $artifacts | Where-Object { Test-Path $_ }

Write-Output "Storing symbols..."

$optionalArgs = @()
if (-not $SkipCompress) {
    $optionalArgs += "--compress"
}

symstore-venv\Scripts\symstore $optionalArgs --skip-published .\SymStore @artifacts
if ($LASTEXITCODE -ne 0) {
    Write-Error "Command exited with code $LASTEXITCODE"
}

Write-Output "Done."
