param(
    [Parameter(Mandatory = $true)]
    [string] $Executable,

    [Parameter(Mandatory = $true)]
    [string] $UserData,

    [ValidateSet('Save', 'NewGame', 'Manual')]
    [string] $Mode = 'Save',

    [string] $Save,
    [string] $Config,
    [string] $LogDirectory,
    [string] $OsgLibraryPath,
    [switch] $NoGrab,
    [ValidateRange(10, 300)]
    [int] $TimeoutSeconds = 60,
    [string] $EvidenceRoot,
    [string] $PythonExecutable = 'python',
    [string[]] $DllDirectory = @(),
    [string] $SourceHead = 'unrecorded',
    [string] $SourceDiffSha256 = 'unrecorded'
)

$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path -LiteralPath $Executable).Path
if ($Mode -eq 'Manual') {
    # Keep manual tests on the same entry point but never apply the legacy
    # timeout/kill policy or move the normal profile's crash dump.
    if ([string]::IsNullOrWhiteSpace($EvidenceRoot)) {
        $EvidenceRoot = Join-Path $UserData 'runtime-qc-evidence'
    }
    $manualArgs = @((Join-Path $PSScriptRoot 'gameplay-diagnostics.py'), 'launch',
        '--executable', $Executable, '--user-config', $UserData,
        '--evidence-root', $EvidenceRoot, '--source-head', $SourceHead,
        '--source-diff-sha256', $SourceDiffSha256)
    foreach ($directory in $DllDirectory) { $manualArgs += @('--dll-directory', $directory) }
    if ($OsgLibraryPath) { $manualArgs += @('--osg-library-path', $OsgLibraryPath) }
    & $PythonExecutable @manualArgs
    if ($LASTEXITCODE -ne 0) { throw "Manual diagnostic collector failed: $LASTEXITCODE" }
    return
}
# The non-manual QC route must inspect the deployed package too, before moving
# dumps or launching. Source/native tests cannot establish package completeness.
& $PythonExecutable (Join-Path $PSScriptRoot 'shader_resources.py') verify (Join-Path (Split-Path $Executable) 'resources/shaders')
if ($LASTEXITCODE -ne 0) { throw 'Shader package QC failed; runtime test not started.' }
if ($Mode -eq 'Save' -and [string]::IsNullOrWhiteSpace($Save)) {
    throw '-Save is required in Save mode.'
}
if ($Mode -eq 'Save') {
    $Save = (Resolve-Path -LiteralPath $Save).Path
}
if ([string]::IsNullOrWhiteSpace($Config)) {
    $Config = $UserData
}
if ([string]::IsNullOrWhiteSpace($LogDirectory)) {
    $LogDirectory = $UserData
}

$null = New-Item -ItemType Directory -Force -Path $UserData
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ([string]::IsNullOrWhiteSpace($EvidenceRoot)) {
    $EvidenceRoot = Join-Path $UserData 'runtime-qc-evidence'
}
$evidence = Join-Path $EvidenceRoot "$timestamp-$($Mode.ToLowerInvariant())"
$null = New-Item -ItemType Directory -Force -Path $evidence

# OpenMW's crash writer does not replace an existing dump. Preserve the old
# artifact with this run's evidence and clear only the exact configured dump
# slot so an access violation cannot be misdiagnosed from stale crash data.
$dump = Join-Path $LogDirectory 'openmw-crash.dmp'
if (Test-Path -LiteralPath $dump) {
    Move-Item -LiteralPath $dump -Destination (Join-Path $evidence 'preexisting-openmw-crash.dmp')
}

$previousStrictQc = $env:OPENMW_V4_STRICT_QC
$previousSuppressDialog = $env:OPENMW_SUPPRESS_FATAL_DIALOG
$previousOsgLibraryPath = $env:OSG_LIBRARY_PATH
$previousRtssDisable = $env:VK_LOADER_LAYERS_DISABLE
$env:OPENMW_V4_STRICT_QC = '1'
$env:OPENMW_SUPPRESS_FATAL_DIALOG = '1'
$env:VK_LOADER_LAYERS_DISABLE = 'VK_LAYER_RTSS'
if (-not [string]::IsNullOrWhiteSpace($OsgLibraryPath)) {
    $env:OSG_LIBRARY_PATH = $OsgLibraryPath
}

$arguments = [System.Collections.Generic.List[string]]::new()
$arguments.Add('--user-data')
$arguments.Add($UserData)
$arguments.Add('--config')
$arguments.Add($Config)
if ($NoGrab) {
    $arguments.Add('--no-grab')
}
if ($Mode -eq 'Save') {
    $arguments.Add('--skip-menu')
    $arguments.Add('--load-savegame')
    $arguments.Add($Save)
} else {
    $arguments.Add('--skip-menu')
    $arguments.Add('--new-game')
}

$start = Get-Date
$timedOut = $false
$exitCode = $null
try {
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.WorkingDirectory = Split-Path $Executable
    $startInfo.UseShellExecute = $false
    foreach ($argument in $arguments) {
        $startInfo.ArgumentList.Add($argument)
    }
    $process = [System.Diagnostics.Process]::Start($startInfo)
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $timedOut = $true
        Stop-Process -Id $process.Id
        $process.WaitForExit()
    }
    if (-not $timedOut) {
        $exitCode = $process.ExitCode
    }
} finally {
    $env:OPENMW_V4_STRICT_QC = $previousStrictQc
    $env:OPENMW_SUPPRESS_FATAL_DIALOG = $previousSuppressDialog
    $env:OSG_LIBRARY_PATH = $previousOsgLibraryPath
    $env:VK_LOADER_LAYERS_DISABLE = $previousRtssDisable
}

$log = Join-Path $LogDirectory 'openmw.log'
if (Test-Path -LiteralPath $log) {
    Copy-Item -LiteralPath $log -Destination (Join-Path $evidence 'openmw.log')
}
if ((Test-Path -LiteralPath $dump) -and (Get-Item -LiteralPath $dump).LastWriteTime -ge $start) {
    Copy-Item -LiteralPath $dump -Destination (Join-Path $evidence 'openmw-crash.dmp')
}

$patterns = @(
    'V4 strict QC',
    'Renderer backend:',
    'Loading cell ',
    'Failed to load saved game',
    'Fatal',
    'Unexpected destruction of LuaWorker',
    'Vulkan record/submit failed',
    'validation error',
    'device lost'
)
$evidenceLines = if (Test-Path -LiteralPath $log) {
    Select-String -LiteralPath $log -SimpleMatch -Pattern $patterns | ForEach-Object { $_.Line }
} else {
    @('openmw.log was not produced')
}
$summary = @(
    "mode=$Mode",
    "executable=$Executable",
    "save=$Save",
    "logDirectory=$LogDirectory",
    "started=$($start.ToString('o'))",
    "timedOut=$timedOut",
    "exitCode=$exitCode",
    "evidence=$evidence",
    '',
    'Selected diagnostic lines:',
    $evidenceLines
)
$summary | Set-Content -LiteralPath (Join-Path $evidence 'runtime-qc-summary.txt') -Encoding UTF8
$summary
