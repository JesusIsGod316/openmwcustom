$ErrorActionPreference = 'Stop'

$GameDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Exe = Join-Path $GameDir 'openmw.exe'
$UserOpenMW = Join-Path $env:USERPROFILE 'Documents\My Games\OpenMW'
$SettingsPath = Join-Path $UserOpenMW 'settings.cfg'
$OpenmwCfgPath = Join-Path $UserOpenMW 'openmw.cfg'
$ProfilesRoot = Join-Path $UserOpenMW 'OptimizedMW-GL-P3-Profiles'

if (-not (Test-Path -LiteralPath $Exe)) { Write-Host 'ERROR: Put this launcher beside openmw.exe.' -ForegroundColor Red; Read-Host 'Press Enter to close'; exit 1 }
if (-not (Test-Path -LiteralPath $SettingsPath)) { Write-Host "ERROR: settings.cfg not found at $SettingsPath" -ForegroundColor Red; Read-Host 'Press Enter to close'; exit 1 }

function Set-IniValue {
    param([string]$Path,[string]$Section,[string]$Key,[string]$Value)
    $lines=[System.Collections.Generic.List[string]]::new()
    foreach($line in [System.IO.File]::ReadAllLines($Path)){[void]$lines.Add($line)}
    $sectionPattern='^\s*\['+[regex]::Escape($Section)+'\]\s*$'
    $keyPattern='^\s*'+[regex]::Escape($Key)+'\s*='
    $sectionIndex=-1
    for($i=0;$i -lt $lines.Count;++$i){if($lines[$i]-match $sectionPattern){$sectionIndex=$i;break}}
    if($sectionIndex -lt 0){
        if($lines.Count -gt 0 -and $lines[$lines.Count-1] -ne ''){[void]$lines.Add('')}
        [void]$lines.Add("[$Section]")
        [void]$lines.Add("$Key = $Value")
    } else {
        $end=$lines.Count
        for($i=$sectionIndex+1;$i -lt $lines.Count;++$i){if($lines[$i]-match '^\s*\[.+\]\s*$'){$end=$i;break}}
        $found=-1
        for($i=$sectionIndex+1;$i -lt $end;++$i){if($lines[$i]-match $keyPattern){$found=$i;break}}
        if($found -ge 0){$lines[$found]="$Key = $Value"}else{$lines.Insert($end,"$Key = $Value")}
    }
    [System.IO.File]::WriteAllLines($Path,$lines,[System.Text.UTF8Encoding]::new($false))
}

Write-Host ''
Write-Host 'OptimizedMW GL-P3 integration-repair benchmark launcher' -ForegroundColor Cyan
Write-Host '  1 = CONTROL       - accepted P1/P2 only; all substantive P3 candidates OFF'
Write-Host '  2 = P3B-1HELPER   - validated template prefetch with one helper'
Write-Host '  3 = P3B-2HELPER   - upgraded dynamically-balanced template prefetch'
Write-Host '  4 = P3C-PREMERGE  - semantic premerge on cancellable distant optional work'
Write-Host '  5 = P3D-DISPLAY   - distant display-list command cache'
Write-Host '  6 = P3E-NORMALIZE - strip material-ignored static vertex-color streams'
Write-Host '  7 = P3F-SHADOW    - distant static shadow proxy batching'
Write-Host '  8 = CORE-CANDIDATE- P3B-2HELPER + P3D + P3F'
Write-Host '  9 = ALL-REPAIRED  - P3B-2HELPER + P3C + P3D + P3E + P3F'
Write-Host ''
do{$choice=Read-Host 'Choose test mode (1-9)'}until($choice -in @('1','2','3','4','5','6','7','8','9'))

$P3A='false'
$P3B='false'
$P3BWorkers='2'
$P3C='false'
$P3D='false'
$P3E='false'
$P3F='false'
$Experiment='CONTROL'
switch($choice){
    '2'{$Experiment='P3B-1HELPER';$P3B='true';$P3BWorkers='1'}
    '3'{$Experiment='P3B-2HELPER';$P3B='true';$P3BWorkers='2'}
    '4'{$Experiment='P3C-OPTIONAL-PREMERGE';$P3C='true'}
    '5'{$Experiment='P3D-DISPLAYLIST';$P3D='true'}
    '6'{$Experiment='P3E-IGNORED-COLOR-STRIP';$P3E='true'}
    '7'{$Experiment='P3F-SHADOW-BATCHING';$P3F='true'}
    '8'{$Experiment='P3-CORE-B2-D-F';$P3B='true';$P3BWorkers='2';$P3D='true';$P3F='true'}
    '9'{$Experiment='P3-ALL-REPAIRED';$P3B='true';$P3BWorkers='2';$P3C='true';$P3D='true';$P3E='true';$P3F='true'}
}

New-Item -ItemType Directory -Path $ProfilesRoot -Force | Out-Null
$stamp=Get-Date -Format 'yyyyMMdd_HHmmss'
$ProfileDir=Join-Path $ProfilesRoot ("OptimizedMW_GL-P3_{0}_{1}" -f $Experiment,$stamp)
New-Item -ItemType Directory -Path $ProfileDir -Force | Out-Null
$BackupSettings=Join-Path $ProfileDir 'settings-before.cfg'
Copy-Item -LiteralPath $SettingsPath -Destination $BackupSettings -Force
$changedSettings=$false

try{
    $changedSettings=$true

    # Accepted P1/P2 benchmark foundation.
    Set-IniValue $SettingsPath 'Cells' 'object paging' 'true'
    Set-IniValue $SettingsPath 'Cells' 'object paging active grid' 'true'
    Set-IniValue $SettingsPath 'Cells' 'ram cache mode' 'overdrive'
    Set-IniValue $SettingsPath 'Cells' 'opimizedmw host pressure' 'true'
    Set-IniValue $SettingsPath 'Cells' 'opimizedmw speculative budget' 'true'
    Set-IniValue $SettingsPath 'Cells' 'opimizedmw transient budget mb' '1024'
    Set-IniValue $SettingsPath 'Cells' 'opimizedmw host reserve mb' '0'
    Set-IniValue $SettingsPath 'Cells' 'opimizedmw commit reserve mb' '1024'
    Set-IniValue $SettingsPath 'Cells' 'opimizedmw paging optimizer' 'true'
    Set-IniValue $SettingsPath 'Cells' 'opimizedmw paging readiness split' 'true'

    # P3A is intentionally parked after runtime showed no surviving primitive work.
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw submission compaction' $P3A
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw parallel template prefetch' $P3B
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw parallel template prefetch workers' $P3BWorkers
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw parallel template prefetch min templates' '16'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw semantic premerge' $P3C
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw distant display lists' $P3D
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw normalized static packets' $P3E
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw shadow static batching' $P3F

    # Low-overhead P3 diagnostic: logs renderingTraversals() handoffs >=20 ms.
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw render handoff attribution' 'true'

    Set-IniValue $SettingsPath 'V3' 'v3.8 world batching mode' '2'
    Set-IniValue $SettingsPath 'V3' 'v3.8 world batching merge multiplier' '1.5'
    Set-IniValue $SettingsPath 'V3' 'v3.8 world batching min instances' '2'
    Set-IniValue $SettingsPath 'V3' 'v3.11 active grid prepare mode' '2'
    Set-IniValue $SettingsPath 'V3' 'v3.12 spatial batch mode' '0'
    Set-IniValue $SettingsPath 'V3' 'v3.13 chunk quality mode' '1'
    Set-IniValue $SettingsPath 'V3' 'v3.15 premerge state canonicalization' 'false'
    Set-IniValue $SettingsPath 'V3' 'v3.15 packetized premerge mode' '0'
    Set-IniValue $SettingsPath 'Camera' 'v3.21 full body first person' 'true'
    Set-IniValue $SettingsPath 'Camera' 'full body first person hybrid animations' 'false'
    Set-IniValue $SettingsPath 'Groundcover' 'density' '1.0'

    Copy-Item -LiteralPath $SettingsPath -Destination (Join-Path $ProfileDir 'settings-effective-test.cfg') -Force
    if(Test-Path -LiteralPath $OpenmwCfgPath){Copy-Item -LiteralPath $OpenmwCfgPath -Destination (Join-Path $ProfileDir 'openmw.cfg') -Force}
    if(Test-Path -LiteralPath (Join-Path $GameDir 'CI-ID.txt')){Copy-Item -LiteralPath (Join-Path $GameDir 'CI-ID.txt') -Destination (Join-Path $ProfileDir 'CI-ID.txt') -Force}

    $exeHash=(Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash
    $settingsHash=(Get-FileHash -LiteralPath $SettingsPath -Algorithm SHA256).Hash
    $cfgHash=if(Test-Path -LiteralPath $OpenmwCfgPath){(Get-FileHash -LiteralPath $OpenmwCfgPath -Algorithm SHA256).Hash}else{'MISSING'}
    @(
        "experiment=$Experiment",
        "expected_lineage=optimizedmw/gl-p3-integration-repair",
        "p3a_submission_compaction=$P3A",
        "p3b_parallel_template_prefetch=$P3B",
        "p3b_workers=$P3BWorkers",
        "p3b_min_templates=16",
        "p3c_semantic_premerge=$P3C",
        "p3d_distant_display_lists=$P3D",
        "p3e_normalized_static_packets=$P3E",
        "p3f_shadow_static_batching=$P3F",
        "diagnostic_render_handoff=true",
        "diagnostic_render_handoff_threshold_ms=20",
        "p1_host_pressure=true",
        "p1_speculative_budget=true",
        "p2_paging_optimizer=true",
        "p2_readiness_split=true",
        "v38_world_batching_mode=2",
        "v311_active_grid_prepare_mode=2",
        "v313_chunk_quality_mode=1",
        "groundcover_density=1.0",
        "full_body_first_person=true",
        "hybrid_animations=false",
        "openmw_exe_sha256=$exeHash",
        "settings_effective_sha256=$settingsHash",
        "openmw_cfg_sha256=$cfgHash"
    ) | Set-Content -LiteralPath (Join-Path $ProfileDir 'TEST_MODE.txt') -Encoding Ascii

    $env:OPENMW_V3_PAGING_FILE=Join-Path $ProfileDir 'v3-paging.csv'
    $env:OPENMW_V3_RENDER_FILE=Join-Path $ProfileDir 'v3-render.csv'
    $env:OPENMW_V3_EVENT_FILE=Join-Path $ProfileDir 'v3-events.csv'
    $env:OPENMW_V3_TRANSITION_FILE=Join-Path $ProfileDir 'v3-transition.csv'
    $env:OPENMW_V3_SHADOW_FILE=Join-Path $ProfileDir 'v3-shadow.csv'
    $env:OPENMW_V36_BATCHING_FILE=Join-Path $ProfileDir 'v36-batching.csv'
    $env:OPENMW_OSG_STATS_FILE=Join-Path $ProfileDir 'v3-osg-stats.log'
    $env:OPENMW_OSG_STATS_LIST='times;resource'

    Write-Host ''
    Write-Host "Starting OptimizedMW GL-P3 integration test: $Experiment" -ForegroundColor Green
    Write-Host 'Use the SAME outdoor save, fixed heavy view, route, and frame-cap state for every comparison.' -ForegroundColor Yellow
    Write-Host 'Hold the same heavy outdoor view ~45 sec, then walk the same 2-3 minute route across several cell boundaries. Avoid doors. Quit normally.'
    Write-Host ''

    $memoryCsv=Join-Path $ProfileDir 'process-memory.csv'
    'epoch_ms,elapsed_s,working_set_mb,private_mb,virtual_mb,cpu_total_s' | Set-Content -LiteralPath $memoryCsv -Encoding Ascii
    $start=[DateTimeOffset]::UtcNow
    $process=Start-Process -FilePath $Exe -WorkingDirectory $GameDir -PassThru
    while(-not $process.HasExited){
        try{
            $process.Refresh()
            $now=[DateTimeOffset]::UtcNow
            $line='{0},{1:F3},{2:F1},{3:F1},{4:F1},{5:F3}' -f $now.ToUnixTimeMilliseconds(),($now-$start).TotalSeconds,($process.WorkingSet64/1MB),($process.PrivateMemorySize64/1MB),($process.VirtualMemorySize64/1MB),$process.TotalProcessorTime.TotalSeconds
            Add-Content -LiteralPath $memoryCsv -Value $line -Encoding Ascii
        }catch{}
        Start-Sleep -Milliseconds 1000
    }
    $process.WaitForExit()
    "exit_code=$($process.ExitCode)" | Add-Content -LiteralPath (Join-Path $ProfileDir 'TEST_MODE.txt') -Encoding Ascii
}
finally{
    foreach($name in @('OPENMW_V3_PAGING_FILE','OPENMW_V3_RENDER_FILE','OPENMW_V3_EVENT_FILE','OPENMW_V3_TRANSITION_FILE','OPENMW_V3_SHADOW_FILE','OPENMW_V36_BATCHING_FILE','OPENMW_OSG_STATS_FILE','OPENMW_OSG_STATS_LIST')){
        Remove-Item ("Env:"+$name) -ErrorAction SilentlyContinue
    }
    if(Test-Path -LiteralPath (Join-Path $UserOpenMW 'openmw.log')){
        Copy-Item -LiteralPath (Join-Path $UserOpenMW 'openmw.log') -Destination (Join-Path $ProfileDir 'openmw.log') -Force -ErrorAction SilentlyContinue
    }
    if($changedSettings -and (Test-Path -LiteralPath $BackupSettings)){
        Copy-Item -LiteralPath $BackupSettings -Destination $SettingsPath -Force
    }
}

$zipPath="$ProfileDir.zip"
if(Test-Path -LiteralPath $zipPath){Remove-Item -LiteralPath $zipPath -Force}
Compress-Archive -Path (Join-Path $ProfileDir '*') -DestinationPath $zipPath -CompressionLevel Optimal
Write-Host ''
Write-Host 'GL-P3 profile complete. Your normal settings have been restored.' -ForegroundColor Green
Write-Host "Upload this ZIP to ChatGPT: $zipPath" -ForegroundColor Cyan
try{Start-Process explorer.exe -ArgumentList "/select,`"$zipPath`""}catch{}
Read-Host 'Press Enter to close'
