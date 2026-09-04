[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildRoot,

    [string]$UserConfigDir = (Join-Path $env:USERPROFILE 'Documents\My Games\OpenMW'),

    [string]$OpenMwCfg,
    [string]$EffectiveSettings,
    [string]$FrozenGamingSettings,
    [string]$GamingWrapper,
    [string]$SaveFile,
    [string]$OutputDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Resolve-OptionalPath {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Get-HashRecord {
    param(
        [string]$Name,
        [string]$Path,
        [bool]$Required = $false
    )

    $resolved = Resolve-OptionalPath $Path
    if ($null -eq $resolved) {
        return [ordered]@{
            name = $Name
            path = $Path
            required = $Required
            exists = $false
            sha256 = $null
            bytes = $null
        }
    }

    $item = Get-Item -LiteralPath $resolved
    return [ordered]@{
        name = $Name
        path = $resolved
        required = $Required
        exists = $true
        sha256 = (Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash.ToLowerInvariant()
        bytes = [int64]$item.Length
    }
}

function Normalize-ConfigValue {
    param([string]$Value)
    $v = $Value.Trim()
    if ($v.Length -ge 2 -and $v.StartsWith('"') -and $v.EndsWith('"')) {
        $v = $v.Substring(1, $v.Length - 2)
    }
    return $v
}

$BuildRoot = (Resolve-Path -LiteralPath $BuildRoot).Path
if ([string]::IsNullOrWhiteSpace($OpenMwCfg)) {
    $OpenMwCfg = Join-Path $UserConfigDir 'openmw.cfg'
}
if ([string]::IsNullOrWhiteSpace($EffectiveSettings)) {
    $EffectiveSettings = Join-Path $UserConfigDir 'settings.cfg'
}
if ([string]::IsNullOrWhiteSpace($FrozenGamingSettings)) {
    $FrozenGamingSettings = Join-Path $BuildRoot 'settings-v325-mode151-gaming.cfg'
}
if ([string]::IsNullOrWhiteSpace($GamingWrapper)) {
    $GamingWrapper = Join-Path $BuildRoot 'V3.25_Mode151_Gaming.bat'
}
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $BuildRoot ('V4_CP0_CAPTURE_' + (Get-Date -Format 'yyyyMMdd_HHmmss'))
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$OutputDir = (Resolve-Path -LiteralPath $OutputDir).Path

$expectedCommit = 'f7557829bcb14e339410cefb32b6612e5009e46d'
$expectedExeHash = '34d7a715e25d92dcad6b20f807e8b44a272fd3382e2d2f0a22e03bedac3e25c2'
$expectedOpenMwCfgHash = 'de4048a4f8766e23f13a9c81eb1960924bec285894f1939a3c91ae184bef63b9'
$expectedBenchmarkSettingsHash = '461d64eab6f5d7b97d6bf008d2c41444c242d1b41d4a93861f6b4089b8f7c304'

$files = @(
    Get-HashRecord -Name 'openmw.exe' -Path (Join-Path $BuildRoot 'openmw.exe') -Required $true
    Get-HashRecord -Name 'V3.25_Mode151_Gaming.bat' -Path $GamingWrapper -Required $true
    Get-HashRecord -Name 'effective settings.cfg' -Path $EffectiveSettings -Required $true
    Get-HashRecord -Name 'frozen gaming settings profile' -Path $FrozenGamingSettings -Required $true
    Get-HashRecord -Name 'openmw.cfg' -Path $OpenMwCfg -Required $true
    Get-HashRecord -Name 'canonical test save' -Path $SaveFile -Required $true
)

$configLines = @()
$dataDirs = @()
$contentEntries = @()
$fallbackArchives = @()
$openMwCfgResolved = Resolve-OptionalPath $OpenMwCfg
if ($null -ne $openMwCfgResolved) {
    foreach ($rawLine in Get-Content -LiteralPath $openMwCfgResolved) {
        $line = $rawLine.Trim()
        if ($line.Length -eq 0 -or $line.StartsWith('#')) { continue }
        if ($line -match '^(data|content|fallback-archive)\s*=\s*(.*)$') {
            $kind = $Matches[1]
            $value = Normalize-ConfigValue $Matches[2]
            $configLines += ('{0}={1}' -f $kind, $value)
            switch ($kind) {
                'data' { $dataDirs += $value }
                'content' { $contentEntries += $value }
                'fallback-archive' { $fallbackArchives += $value }
            }
        }
    }
}

$manifestPath = Join-Path $OutputDir 'cp0-content-manifest.txt'
@(
    '# OpenMW Custom Build V4 CP0 content/load-order manifest'
    '# Generated from the effective openmw.cfg; order is preserved.'
    ('# Generated: ' + (Get-Date).ToString('o'))
    ''
    $configLines
) | Set-Content -LiteralPath $manifestPath -Encoding UTF8
$manifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()

$resolvedContent = @()
foreach ($content in $contentEntries) {
    $found = $null
    # OpenMW later data directories have higher VFS priority, so search in reverse order.
    for ($i = $dataDirs.Count - 1; $i -ge 0; --$i) {
        $candidateRoot = [Environment]::ExpandEnvironmentVariables($dataDirs[$i])
        if (-not [System.IO.Path]::IsPathRooted($candidateRoot)) { continue }
        $candidate = Join-Path $candidateRoot $content
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $found = (Resolve-Path -LiteralPath $candidate).Path
            break
        }
    }

    if ($null -eq $found) {
        $resolvedContent += [ordered]@{ content = $content; path = $null; sha256 = $null; bytes = $null }
    }
    else {
        $item = Get-Item -LiteralPath $found
        $resolvedContent += [ordered]@{
            content = $content
            path = $found
            sha256 = (Get-FileHash -LiteralPath $found -Algorithm SHA256).Hash.ToLowerInvariant()
            bytes = [int64]$item.Length
        }
    }
}

$envNames = @(
    'OPENMW_V319_FOCUS_CADENCE',
    'OPENMW_V320_FOCUS_ADAPTIVE',
    'OPENMW_V320_ENGINE_LUA_FASTPATHS',
    'OPENMW_V320_SOUND_CONVERSION_CACHE',
    'OPENMW_V320_SOUND_QUERY_COALESCING',
    'OPENMW_V320_LUA_PROFILER_CAPABLE',
    'OPENMW_V321_COMPLETION_GOVERNOR',
    'OPENMW_V321_CP2_FAIRNESS',
    'OPENMW_V321_CP3_FULL_BODY_FIRST_PERSON',
    'OPENMW_V321_CP4_SHADOW_COMPAT',
    'OPENMW_V322_CP1_MSOC_HOT_PATH',
    'OPENMW_V322_CP2_OCCLUDER_EFFICIENCY_MODE',
    'OPENMW_V322_PARALLEL_ACTOR_AVOIDANCE',
    'OPENMW_V323_PARALLEL_MSOC_MODE',
    'OPENMW_V324_FRAME_JOB_QOS',
    'OPENMW_V324_ASYNC_MSOC',
    'OPENMW_V324_DEEP_TELEMETRY',
    'OPENMW_V325_ACTOR_SOURCE_BATCH',
    'OPENMW_V325_PARALLEL_ACTOR_BINDING',
    'OSG_THREADING'
)
$environment = [ordered]@{}
foreach ($name in $envNames) {
    $environment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

$cpu = $null
$gpu = @()
$os = $null
$computer = $null
try { $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1 Name, NumberOfCores, NumberOfLogicalProcessors } catch {}
try { $gpu = @(Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion, AdapterRAM) } catch {}
try { $os = Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber, TotalVisibleMemorySize, FreePhysicalMemory } catch {}
try { $computer = Get-CimInstance Win32_ComputerSystem | Select-Object TotalPhysicalMemory, Manufacturer, Model } catch {}

$exeRecord = $files | Where-Object { $_.name -eq 'openmw.exe' }
$cfgRecord = $files | Where-Object { $_.name -eq 'openmw.cfg' }
$effectiveSettingsRecord = $files | Where-Object { $_.name -eq 'effective settings.cfg' }

$checks = [ordered]@{
    expected_final_commit = $expectedCommit
    openmw_exe_matches_final_build = ($exeRecord.exists -and $exeRecord.sha256 -eq $expectedExeHash)
    openmw_cfg_matches_mode151_ab = ($cfgRecord.exists -and $cfgRecord.sha256 -eq $expectedOpenMwCfgHash)
    effective_settings_matches_mode151_ab = ($effectiveSettingsRecord.exists -and $effectiveSettingsRecord.sha256 -eq $expectedBenchmarkSettingsHash)
    all_required_files_present = (($files | Where-Object { $_.required -and -not $_.exists }).Count -eq 0)
}

$capture = [ordered]@{
    schema = 'OMW_V4_CP0_CAPTURE_1'
    generated_utc = (Get-Date).ToUniversalTime().ToString('o')
    event = 'evt.v4.0.cp0.freeze_source_lock.002'
    final_v325_commit = $expectedCommit
    freeze_ref = 'v3.25-final-freeze'
    cp0_branch = 'v4.0-cp0-freeze'
    build_root = $BuildRoot
    user_config_dir = $UserConfigDir
    files = $files
    openmw_cfg_manifest = [ordered]@{
        path = $manifestPath
        sha256 = $manifestHash
        data_directories = $dataDirs
        fallback_archives = $fallbackArchives
        content = $resolvedContent
    }
    process_environment = $environment
    hardware = [ordered]@{
        computer = $computer
        cpu = $cpu
        gpu = $gpu
        os = $os
    }
    verification = $checks
}

$jsonPath = Join-Path $OutputDir 'cp0-freeze-capture.json'
$capture | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $jsonPath -Encoding UTF8
$jsonHash = (Get-FileHash -LiteralPath $jsonPath -Algorithm SHA256).Hash.ToLowerInvariant()

$summaryPath = Join-Path $OutputDir 'cp0-freeze-summary.txt'
$summary = @(
    'OpenMW Custom Build V4 CP0 freeze capture'
    ('Generated UTC: ' + $capture.generated_utc)
    ('Final V3.25 commit: ' + $expectedCommit)
    ('Capture JSON SHA256: ' + $jsonHash)
    ('Content manifest SHA256: ' + $manifestHash)
    ''
    ('openmw.exe expected hash match: ' + $checks.openmw_exe_matches_final_build)
    ('openmw.cfg matches accepted Mode151 A/B: ' + $checks.openmw_cfg_matches_mode151_ab)
    ('effective settings matches accepted Mode151 A/B: ' + $checks.effective_settings_matches_mode151_ab)
    ('all required files present: ' + $checks.all_required_files_present)
    ''
    'Files:'
)
foreach ($record in $files) {
    $summary += ('- {0}: exists={1}; sha256={2}; path={3}' -f $record.name, $record.exists, $record.sha256, $record.path)
}
$summary += ''
$summary += 'Next: run one clean Mode151-only V3_Unified_Test capture with frame pacer OFF/nonbinding and deep telemetry OFF, then archive this capture directory with the benchmark bundle and reference screenshots.'
$summary | Set-Content -LiteralPath $summaryPath -Encoding UTF8

Write-Host ''
Write-Host 'V4 CP0 capture complete.'
Write-Host ('Output: ' + $OutputDir)
Write-Host ('Capture JSON: ' + $jsonPath)
Write-Host ('Content manifest: ' + $manifestPath)
Write-Host ('Summary: ' + $summaryPath)
Write-Host ''
Write-Host ('openmw.exe matches final V3.25 build: ' + $checks.openmw_exe_matches_final_build)
Write-Host ('openmw.cfg matches accepted Mode151 A/B: ' + $checks.openmw_cfg_matches_mode151_ab)
Write-Host ('effective settings matches accepted Mode151 A/B: ' + $checks.effective_settings_matches_mode151_ab)
Write-Host ('all required files present: ' + $checks.all_required_files_present)
