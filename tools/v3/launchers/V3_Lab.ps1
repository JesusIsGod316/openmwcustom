param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('City','Transition','Render')]
    [string]$Mode
)

$ErrorActionPreference = 'Stop'
$GameDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Exe = Join-Path $GameDir 'openmw.exe'
$UserOpenMW = Join-Path $env:USERPROFILE 'Documents\My Games\OpenMW'
$SettingsPath = Join-Path $UserOpenMW 'settings.cfg'
$OpenmwCfgPath = Join-Path $UserOpenMW 'openmw.cfg'
$ProfilesRoot = Join-Path $UserOpenMW 'V3Profiles'

if (-not (Test-Path -LiteralPath $Exe)) {
    Write-Host 'ERROR: This launcher must be beside openmw.exe.' -ForegroundColor Red
    Read-Host 'Press Enter to close'
    exit 1
}
if (-not (Test-Path -LiteralPath $SettingsPath)) {
    Write-Host "ERROR: settings.cfg was not found at $SettingsPath" -ForegroundColor Red
    Read-Host 'Press Enter to close'
    exit 1
}

function Set-IniValue {
    param([string]$Path, [string]$Section, [string]$Key, [string]$Value)
    $lines = [System.Collections.Generic.List[string]]::new()
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) { [void]$lines.Add($line) }

    $sectionPattern = '^\s*\[' + [regex]::Escape($Section) + '\]\s*$'
    $keyPattern = '^\s*' + [regex]::Escape($Key) + '\s*='
    $sectionIndex = -1
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -match $sectionPattern) { $sectionIndex = $i; break }
    }

    if ($sectionIndex -lt 0) {
        if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -ne '') { [void]$lines.Add('') }
        [void]$lines.Add("[$Section]")
        [void]$lines.Add("$Key = $Value")
    }
    else {
        $end = $lines.Count
        for ($i = $sectionIndex + 1; $i -lt $lines.Count; ++$i) {
            if ($lines[$i] -match '^\s*\[.+\]\s*$') { $end = $i; break }
        }
        $found = -1
        for ($i = $sectionIndex + 1; $i -lt $end; ++$i) {
            if ($lines[$i] -match $keyPattern) { $found = $i; break }
        }
        if ($found -ge 0) { $lines[$found] = "$Key = $Value" }
        else { $lines.Insert($end, "$Key = $Value") }
    }

    [System.IO.File]::WriteAllLines($Path, $lines, [System.Text.UTF8Encoding]::new($false))
}

function Write-HardwareSnapshot {
    param([string]$Path)
    $out = [System.Collections.Generic.List[string]]::new()
    [void]$out.Add("Captured: $([DateTimeOffset]::Now.ToString('o'))")
    [void]$out.Add('')
    [void]$out.Add('=== CPU ===')
    try {
        foreach ($cpu in Get-CimInstance Win32_Processor) {
            [void]$out.Add("$($cpu.Name) | cores=$($cpu.NumberOfCores) logical=$($cpu.NumberOfLogicalProcessors) maxMHz=$($cpu.MaxClockSpeed)")
        }
    } catch { [void]$out.Add($_.Exception.Message) }
    [void]$out.Add('')
    [void]$out.Add('=== MEMORY ===')
    try {
        $os = Get-CimInstance Win32_OperatingSystem
        [void]$out.Add("TotalVisibleMemoryMB=$([math]::Round($os.TotalVisibleMemorySize / 1024, 1))")
        [void]$out.Add("FreePhysicalMemoryMB=$([math]::Round($os.FreePhysicalMemory / 1024, 1))")
    } catch { [void]$out.Add($_.Exception.Message) }
    [void]$out.Add('')
    [void]$out.Add('=== GPU ===')
    try {
        foreach ($gpu in Get-CimInstance Win32_VideoController) {
            [void]$out.Add("$($gpu.Name) | Driver=$($gpu.DriverVersion) | AdapterRAM=$($gpu.AdapterRAM)")
        }
    } catch { [void]$out.Add($_.Exception.Message) }
    [void]$out.Add('')
    [void]$out.Add('=== OS ===')
    try {
        $os = Get-CimInstance Win32_OperatingSystem
        [void]$out.Add("$($os.Caption) $($os.Version) build $($os.BuildNumber)")
    } catch { [void]$out.Add($_.Exception.Message) }
    [void]$out.Add('')
    [void]$out.Add('=== ACTIVE POWER PLAN ===')
    try { [void]$out.Add((powercfg /getactivescheme | Out-String).Trim()) } catch { [void]$out.Add($_.Exception.Message) }
    [void]$out.Add('')
    [void]$out.Add('=== NVIDIA-SMI SNAPSHOT ===')
    try {
        if (Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue) {
            [void]$out.Add((& nvidia-smi.exe | Out-String).Trim())
        } else { [void]$out.Add('nvidia-smi not found') }
    } catch { [void]$out.Add($_.Exception.Message) }
    [System.IO.File]::WriteAllLines($Path, $out, [System.Text.UTF8Encoding]::new($false))
}

$Experiment = 'current-settings'
$Prepared = $null
$Scheduler = $null
if ($Mode -ne 'Render') {
    Write-Host ''
    Write-Host 'Choose the runtime experiment for this test:' -ForegroundColor Cyan
    Write-Host '  1 = Baseline Overdrive (new diagnostics, experiments off)'
    Write-Host '  2 = Prepared static instances'
    Write-Host '  3 = Adaptive predictive preload'
    Write-Host '  4 = Prepared instances + adaptive preload'
    do { $choice = Read-Host 'Enter 1, 2, 3, or 4' } until ($choice -in @('1','2','3','4'))
    switch ($choice) {
        '1' { $Experiment = 'baseline'; $Prepared = 'false'; $Scheduler = 'off' }
        '2' { $Experiment = 'prepared'; $Prepared = 'true'; $Scheduler = 'off' }
        '3' { $Experiment = 'adaptive'; $Prepared = 'false'; $Scheduler = 'adaptive' }
        '4' { $Experiment = 'combined'; $Prepared = 'true'; $Scheduler = 'adaptive' }
    }
}

$Stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$ProfileDir = Join-Path $ProfilesRoot ("V3_{0}_{1}_{2}" -f $Mode, $Experiment, $Stamp)
New-Item -ItemType Directory -Force -Path $ProfileDir | Out-Null
$BackupSettings = Join-Path $ProfileDir 'settings-before-test.cfg'
Copy-Item -LiteralPath $SettingsPath -Destination $BackupSettings -Force
if (Test-Path -LiteralPath $OpenmwCfgPath) { Copy-Item -LiteralPath $OpenmwCfgPath -Destination (Join-Path $ProfileDir 'openmw.cfg') -Force }
if (Test-Path -LiteralPath (Join-Path $GameDir 'CI-ID.txt')) { Copy-Item -LiteralPath (Join-Path $GameDir 'CI-ID.txt') -Destination (Join-Path $ProfileDir 'CI-ID.txt') -Force }

$exeHash = (Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash
$manifest = @(
    "mode=$Mode",
    "experiment=$Experiment",
    "timestamp=$([DateTimeOffset]::Now.ToString('o'))",
    "openmw_exe_sha256=$exeHash",
    "game_dir=$GameDir"
)
[System.IO.File]::WriteAllLines((Join-Path $ProfileDir 'TEST_MODE.txt'), $manifest, [System.Text.UTF8Encoding]::new($false))
Write-HardwareSnapshot (Join-Path $ProfileDir 'hardware.txt')

# Remove any inherited profiler variables before selecting this test's streams.
$allVars = @(
    'OPENMW_V3_HITCH_FILE','OPENMW_V3_FRAME_FILE','OPENMW_V3_TELEMETRY_FILE','OPENMW_V3_EVENT_FILE',
    'OPENMW_V3_LUASYNC_FILE','OPENMW_V3_LUA_ACTION_FILE','OPENMW_V3_LUA_UPDATE_FILE','OPENMW_V3_TRANSITION_FILE',
    'OPENMW_V3_PAGING_FILE','OPENMW_V3_RESOURCE_FILE','OPENMW_V3_NAV_FILE','OPENMW_V3_INSERT_FILE',
    'OPENMW_V3_WORKQUEUE_FILE','OPENMW_V3_RENDER_FILE','OPENMW_V3_POSTFX_FILE','OPENMW_V3_STREAMING_FILE',
    'OPENMW_V3_TRACE_FILE','OPENMW_V3_MSOC_DETAIL_FILE','OPENMW_V3_SHADOW_FILE','OPENMW_OSG_STATS_FILE','OPENMW_OSG_STATS_LIST'
)
foreach ($name in $allVars) { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }

$env:OPENMW_V3_HITCH_FILE = Join-Path $ProfileDir 'v3-hitch.csv'
$env:OPENMW_V3_FRAME_FILE = Join-Path $ProfileDir 'v3-frames.csv'

if ($Mode -eq 'City') {
    $env:OPENMW_V3_EVENT_FILE = Join-Path $ProfileDir 'v3-events.csv'
    $env:OPENMW_V3_PAGING_FILE = Join-Path $ProfileDir 'v3-paging.csv'
    $env:OPENMW_V3_STREAMING_FILE = Join-Path $ProfileDir 'v3-streaming.csv'
}
elseif ($Mode -eq 'Transition') {
    $env:OPENMW_V3_TELEMETRY_FILE = Join-Path $ProfileDir 'v3-occlusion.csv'
    $env:OPENMW_V3_EVENT_FILE = Join-Path $ProfileDir 'v3-events.csv'
    $env:OPENMW_V3_LUASYNC_FILE = Join-Path $ProfileDir 'v3-luasync.csv'
    $env:OPENMW_V3_LUA_ACTION_FILE = Join-Path $ProfileDir 'v3-lua-actions.csv'
    $env:OPENMW_V3_LUA_UPDATE_FILE = Join-Path $ProfileDir 'v3-lua-update.csv'
    $env:OPENMW_V3_TRANSITION_FILE = Join-Path $ProfileDir 'v3-transition.csv'
    $env:OPENMW_V3_PAGING_FILE = Join-Path $ProfileDir 'v3-paging.csv'
    $env:OPENMW_V3_RESOURCE_FILE = Join-Path $ProfileDir 'v3-resource.csv'
    $env:OPENMW_V3_NAV_FILE = Join-Path $ProfileDir 'v3-nav.csv'
    $env:OPENMW_V3_INSERT_FILE = Join-Path $ProfileDir 'v3-insertion.csv'
    $env:OPENMW_V3_WORKQUEUE_FILE = Join-Path $ProfileDir 'v3-workqueue.csv'
    $env:OPENMW_V3_RENDER_FILE = Join-Path $ProfileDir 'v3-render.csv'
    $env:OPENMW_V3_STREAMING_FILE = Join-Path $ProfileDir 'v3-streaming.csv'
    $env:OPENMW_V3_TRACE_FILE = Join-Path $ProfileDir 'v3-trace.csv'
    $env:OPENMW_OSG_STATS_FILE = Join-Path $ProfileDir 'v3-osg-stats.log'
    $env:OPENMW_OSG_STATS_LIST = 'times;resource'
}
else {
    $env:OPENMW_V3_TELEMETRY_FILE = Join-Path $ProfileDir 'v3-occlusion.csv'
    $env:OPENMW_V3_PAGING_FILE = Join-Path $ProfileDir 'v3-paging.csv'
    $env:OPENMW_V3_RENDER_FILE = Join-Path $ProfileDir 'v3-render.csv'
    $env:OPENMW_V3_POSTFX_FILE = Join-Path $ProfileDir 'v3-postfx.csv'
    $env:OPENMW_V3_MSOC_DETAIL_FILE = Join-Path $ProfileDir 'v3-msoc-detail.csv'
    $env:OPENMW_V3_SHADOW_FILE = Join-Path $ProfileDir 'v3-shadow.csv'
    $env:OPENMW_OSG_STATS_FILE = Join-Path $ProfileDir 'v3-osg-stats.log'
    $env:OPENMW_OSG_STATS_LIST = 'times;resource'
}

$changedSettings = $false
try {
    if ($Mode -ne 'Render') {
        Set-IniValue $SettingsPath 'Cells' 'ram cache mode' 'overdrive'
        Set-IniValue $SettingsPath 'Cells' 'ram cache overdrive preload' 'balanced'
        Set-IniValue $SettingsPath 'Cells' 'v3 streaming scheduler' $Scheduler
        Set-IniValue $SettingsPath 'Cells' 'v3 streaming target frametime' '25'
        Set-IniValue $SettingsPath 'Cells' 'v3 prepared instance cache' $Prepared
        Set-IniValue $SettingsPath 'Cells' 'v3 prepared instance cache max' '8192'
        $changedSettings = $true
    }
    Copy-Item -LiteralPath $SettingsPath -Destination (Join-Path $ProfileDir 'settings-effective-test.cfg') -Force

    Write-Host ''
    Write-Host "V3 $Mode test: $Experiment" -ForegroundColor Green
    if ($Mode -eq 'City') {
        Write-Host 'Suggested route: use the same outdoor city save, wait 15-20 sec, then walk the same 2-3 minute route across several cell boundaries. Avoid doors.'
    }
    elseif ($Mode -eq 'Transition') {
        Write-Host 'Suggested route: same save outside Anvil -> walk 30 sec -> enter Fighters Guild 20 sec -> exit 20 sec -> re-enter 10 sec -> exit 20 sec -> quit.'
    }
    else {
        Write-Host 'Suggested route: stay in one representative outdoor area for 45-60 sec. Move/rotate normally, but avoid doors and long-distance travel.'
    }
    Write-Host ''

    $memoryCsv = Join-Path $ProfileDir 'v3-process-memory.csv'
    'epoch_ms,elapsed_s,working_set_mb,private_mb,virtual_mb,cpu_total_s' | Set-Content -LiteralPath $memoryCsv -Encoding Ascii
    $start = [DateTimeOffset]::UtcNow
    $process = Start-Process -FilePath $Exe -WorkingDirectory $GameDir -PassThru
    while (-not $process.HasExited) {
        try {
            $process.Refresh()
            $now = [DateTimeOffset]::UtcNow
            $line = '{0},{1:F3},{2:F1},{3:F1},{4:F1},{5:F3}' -f $now.ToUnixTimeMilliseconds(), ($now - $start).TotalSeconds,
                ($process.WorkingSet64 / 1MB), ($process.PrivateMemorySize64 / 1MB), ($process.VirtualMemorySize64 / 1MB),
                $process.TotalProcessorTime.TotalSeconds
            Add-Content -LiteralPath $memoryCsv -Value $line -Encoding Ascii
        } catch {}
        Start-Sleep -Milliseconds 1000
    }
    $process.WaitForExit()
}
finally {
    if (Test-Path -LiteralPath (Join-Path $UserOpenMW 'openmw.log')) {
        Copy-Item -LiteralPath (Join-Path $UserOpenMW 'openmw.log') -Destination (Join-Path $ProfileDir 'openmw.log') -Force -ErrorAction SilentlyContinue
    }
    if ($changedSettings -and (Test-Path -LiteralPath $BackupSettings)) {
        Copy-Item -LiteralPath $BackupSettings -Destination $SettingsPath -Force
    }
}

$zipPath = "$ProfileDir.zip"
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Compress-Archive -Path (Join-Path $ProfileDir '*') -DestinationPath $zipPath -CompressionLevel Optimal
Write-Host ''
Write-Host 'Profile complete and settings restored.' -ForegroundColor Green
Write-Host "Upload this ZIP: $zipPath" -ForegroundColor Cyan
try { Start-Process explorer.exe -ArgumentList "/select,`"$zipPath`"" } catch {}
Read-Host 'Press Enter to close'
