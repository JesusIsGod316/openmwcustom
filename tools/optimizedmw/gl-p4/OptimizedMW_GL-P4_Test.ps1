$ErrorActionPreference = 'Stop'

$GameDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Exe = Join-Path $GameDir 'openmw.exe'
$UserOpenMW = Join-Path $env:USERPROFILE 'Documents\My Games\OpenMW'
$SettingsPath = Join-Path $UserOpenMW 'settings.cfg'
$OpenmwCfgPath = Join-Path $UserOpenMW 'openmw.cfg'
$ProfilesRoot = Join-Path $UserOpenMW 'OptimizedMW-GL-P4-Profiles'

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
Write-Host 'OptimizedMW GL-P4 compile scheduler benchmark launcher' -ForegroundColor Cyan
Write-Host '  1 = CONTROL-B1              - current ICO + validated P3B one-helper'
Write-Host '  2 = CONTROL-B1-C            - current ICO + P3B one-helper + P3C'
Write-Host '  3 = CONTROL-B1-C-D          - current ICO + P3B one-helper + P3C + P3D'
Write-Host '  4 = P4A-B1                  - cost-aware compile scheduler + B1'
Write-Host '  5 = P4B-B1                  - handoff/credit-aware scheduler + B1'
Write-Host '  6 = P4B-B1-C                - handoff scheduler + B1 + C'
Write-Host '  7 = P4B-B1-C-D              - handoff scheduler + B1 + C + D'
Write-Host '  8 = P4B-B1-C-COMPLETION     - mode 6 + V3.21 completed-set governor'
Write-Host '  9 = P4A-B1-C                - cost-aware scheduler + B1 + C'
Write-Host ''
do{$choice=Read-Host 'Choose test mode (1-9)'}until($choice -in @('1','2','3','4','5','6','7','8','9'))

$SchedulerMode='0'
$P3C='false'
$P3D='false'
$Completion='0'
$Experiment='CONTROL-B1'

switch($choice){
    '2'{$Experiment='CONTROL-B1-C';$P3C='true'}
    '3'{$Experiment='CONTROL-B1-C-D';$P3C='true';$P3D='true'}
    '4'{$Experiment='P4A-B1';$SchedulerMode='1'}
    '5'{$Experiment='P4B-B1';$SchedulerMode='2'}
    '6'{$Experiment='P4B-B1-C';$SchedulerMode='2';$P3C='true'}
    '7'{$Experiment='P4B-B1-C-D';$SchedulerMode='2';$P3C='true';$P3D='true'}
    '8'{$Experiment='P4B-B1-C-COMPLETION';$SchedulerMode='2';$P3C='true';$Completion='1'}
    '9'{$Experiment='P4A-B1-C';$SchedulerMode='1';$P3C='true'}
}

New-Item -ItemType Directory -Path $ProfilesRoot -Force | Out-Null
$stamp=Get-Date -Format 'yyyyMMdd_HHmmss'
$ProfileDir=Join-Path $ProfilesRoot ("OptimizedMW_GL-P4_{0}_{1}" -f $Experiment,$stamp)
New-Item -ItemType Directory -Path $ProfileDir -Force | Out-Null
$BackupSettings=Join-Path $ProfileDir 'settings-before.cfg'
Copy-Item -LiteralPath $SettingsPath -Destination $BackupSettings -Force
$changedSettings=$false

try{
    $changedSettings=$true

    # Accepted P1/P2 foundation.
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

    # Freeze P3 around the proven B1 foundation. A/E/F remain parked/off.
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw submission compaction' 'false'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw parallel template prefetch' 'true'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw parallel template prefetch workers' '1'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw parallel template prefetch min templates' '16'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw semantic premerge' $P3C
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw distant display lists' $P3D
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw normalized static packets' 'false'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw shadow static batching' 'false'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw render handoff attribution' 'true'

    # P4 scheduler. Mode 0 constructs the original OSG ICO.
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw compile scheduler mode' $SchedulerMode
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw compile scheduler max budget ms' '2.0'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw compile scheduler credit cap ms' '2.5'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw compile scheduler headroom ratio' '0.25'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw compile scheduler handoff threshold ms' '20.0'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw compile scheduler max queue age frames' '12'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw compile scheduler delete budget ms' '0.15'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw compile scheduler max objects per frame' '4'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw compile scheduler diagnostic threshold ms' '0.15'

    # Freeze the pre-existing compile policy so control and candidate differ only by P4.
    Set-IniValue $SettingsPath 'Cells' 'target framerate' '60'
    Set-IniValue $SettingsPath 'V3' 'v3.8 compile pacing mode' '3'
    Set-IniValue $SettingsPath 'V3' 'v3.15 adaptive compile governor' '1'
    Set-IniValue $SettingsPath 'V3' 'v3.21 completion governor mode' $Completion
    Set-IniValue $SettingsPath 'V3' 'v3.21 CP2 fairness mode' '0'
    Set-IniValue $SettingsPath 'V3' 'v3.21 compile objects per frame' '4'
    Set-IniValue $SettingsPath 'V3' 'v3.21 merge sets per frame' '2'
    Set-IniValue $SettingsPath 'V3' 'v3.21 max deferred frames' '4'
    Set-IniValue $SettingsPath 'V3' 'v3.21 forced merge sets' '2'
    Set-IniValue $SettingsPath 'V3' 'v3.21 compile minimum milliseconds' '1.0'
    Set-IniValue $SettingsPath 'V3' 'v3.21 compile conservative ratio' '0.25'

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
        "expected_lineage=optimizedmw/gl-p4-compile-scheduler",
        "p3b_parallel_template_prefetch=true",
        "p3b_workers=1",
        "p3c_semantic_premerge=$P3C",
        "p3d_distant_display_lists=$P3D",
        "p4_compile_scheduler_mode=$SchedulerMode",
        "p4_max_budget_ms=2.0",
        "p4_credit_cap_ms=2.5",
        "p4_headroom_ratio=0.25",
        "p4_handoff_threshold_ms=20.0",
        "p4_max_queue_age_frames=12",
        "p4_delete_budget_ms=0.15",
        "p4_max_objects_per_frame=4",
        "completion_governor=$Completion",
        "v38_compile_pacing_mode=3",
        "v315_adaptive_compile_governor=1",
        "diagnostic_render_handoff=true",
        "diagnostic_compile_ops=true",
        "openmw_exe_sha256=$exeHash",
        "settings_effective_sha256=$settingsHash",
        "openmw_cfg_sha256=$cfgHash"
    ) | Set-Content -LiteralPath (Join-Path $ProfileDir 'TEST_MODE.txt') -Encoding Ascii

    # Focused nonblocking diagnostics. Avoid deep trace/profilers in performance runs.
    $env:OPENMW_V3_PAGING_FILE=Join-Path $ProfileDir 'v3-paging.csv'
    $env:OPENMW_V3_RENDER_FILE=Join-Path $ProfileDir 'v3-render.csv'
    $env:OPENMW_V3_EVENT_FILE=Join-Path $ProfileDir 'v3-events.csv'
    $env:OPENMW_V3_TRANSITION_FILE=Join-Path $ProfileDir 'v3-transition.csv'
    $env:OPENMW_V3_RESOURCE_FILE=Join-Path $ProfileDir 'v3-resource.csv'
    $env:OPENMW_V3_STREAMING_FILE=Join-Path $ProfileDir 'v3-streaming.csv'
    $env:OPENMW_V3_SHADOW_FILE=Join-Path $ProfileDir 'v3-shadow.csv'
    $env:OPENMW_V36_BATCHING_FILE=Join-Path $ProfileDir 'v36-batching.csv'
    $env:OPENMW_V32_GPU_MEMORY_FILE=Join-Path $ProfileDir 'v3-gpu-memory.csv'
    $env:OPENMW_P4_COMPILE_FILE=Join-Path $ProfileDir 'p4-compile.csv'
    $env:OPENMW_OSG_STATS_FILE=Join-Path $ProfileDir 'v3-osg-stats.log'
    $env:OPENMW_OSG_STATS_LIST='times;resource'

    Write-Host ''
    Write-Host "Starting OptimizedMW GL-P4 test: $Experiment" -ForegroundColor Green
    Write-Host 'Use the SAME outdoor save, fixed heavy view, walking route, and frame-cap state.' -ForegroundColor Yellow
    Write-Host 'Hold the fixed view ~45 sec, then walk the same 2-3 minute route across the same cell boundaries. Quit normally.'
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
    foreach($name in @(
        'OPENMW_V3_PAGING_FILE','OPENMW_V3_RENDER_FILE','OPENMW_V3_EVENT_FILE','OPENMW_V3_TRANSITION_FILE',
        'OPENMW_V3_RESOURCE_FILE','OPENMW_V3_STREAMING_FILE','OPENMW_V3_SHADOW_FILE','OPENMW_V36_BATCHING_FILE',
        'OPENMW_V32_GPU_MEMORY_FILE','OPENMW_P4_COMPILE_FILE','OPENMW_OSG_STATS_FILE','OPENMW_OSG_STATS_LIST'
    )){Remove-Item ("Env:"+$name) -ErrorAction SilentlyContinue}

    if(Test-Path -LiteralPath (Join-Path $UserOpenMW 'openmw.log')){Copy-Item -LiteralPath (Join-Path $UserOpenMW 'openmw.log') -Destination (Join-Path $ProfileDir 'openmw.log') -Force -ErrorAction SilentlyContinue}
    if($changedSettings -and (Test-Path -LiteralPath $BackupSettings)){Copy-Item -LiteralPath $BackupSettings -Destination $SettingsPath -Force}
}

$zipPath="$ProfileDir.zip"
if(Test-Path -LiteralPath $zipPath){Remove-Item -LiteralPath $zipPath -Force}
Compress-Archive -Path (Join-Path $ProfileDir '*') -DestinationPath $zipPath -CompressionLevel Optimal
Write-Host ''
Write-Host 'GL-P4 profile complete. Your normal settings have been restored.' -ForegroundColor Green
Write-Host "Upload this ZIP to ChatGPT: $zipPath" -ForegroundColor Cyan
try{Start-Process explorer.exe -ArgumentList "/select,`"$zipPath`""}catch{}
Read-Host 'Press Enter to close'
