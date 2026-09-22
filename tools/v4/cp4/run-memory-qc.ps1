param(
    [string] $Executable = (Join-Path $PSScriptRoot 'openmw.exe'),
    [string] $UserConfig = (Join-Path $HOME 'Documents/My Games/OpenMW'),
    [ValidateSet('off', 'standard', 'focused')]
    [string] $Diagnostics = 'standard',
    [string] $PythonExecutable = 'python',
    [string] $EvidenceRoot,
    [string] $SourceHead = 'unrecorded'
)
$ErrorActionPreference = 'Stop'
if (-not $EvidenceRoot) { $EvidenceRoot = Join-Path $UserConfig 'runtime-qc-evidence' }
& $PythonExecutable (Join-Path $PSScriptRoot 'gameplay-diagnostics.py') launch `
    --executable $Executable --user-config $UserConfig --evidence-root $EvidenceRoot `
    --diagnostics $Diagnostics --source-head $SourceHead
if ($LASTEXITCODE -ne 0) { throw "Diagnostic helper failed: $LASTEXITCODE" }
