$ErrorActionPreference = 'Stop'

$GameDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Exe = Join-Path $GameDir 'openmw.exe'
$UserOpenMW = Join-Path $env:USERPROFILE 'Documents\My Games\OpenMW'
$SettingsPath = Join-Path $UserOpenMW 'settings.cfg'
$OpenmwCfgPath = Join-Path $UserOpenMW 'openmw.cfg'
$ProfilesRoot = Join-Path $UserOpenMW 'OptimizedMW-GL-P7-Profiles'

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
Write-Host 'OptimizedMW P7 CPU-preparation benchmark launcher' -ForegroundColor Cyan
Write-Host '  1 = P6-QUARANTINE-CONTROL        - retained stutter control, P7 CPU prep off'
Write-Host '  2 = P7-ACTOR-BATCH               - accepted V3.25 actor controller batching'
Write-Host '  3 = P7-TERRAIN-CPU               - two-way vertex + blend/layer CPU preparation'
Write-Host '  4 = P7-ACTOR-TERRAIN             - actor batching + terrain CPU preparation'
Write-Host '  5 = P7-ACTOR-TERRAIN-RESOURCES   - mode 4 + P6R native terrain resource phases'
Write-Host '  6 = P7-FULL-STUTTER              - mode 5 + split terrain VBO uploads'
Write-Host ''
do{$choice=Read-Host 'Choose test mode (1-6)'}until($choice -in @('1','2','3','4','5','6'))

$SchedulerMode='2'
$P3C='true'
$P3D='false'
$Completion='0'
$HeavyMode='0'
$TerrainPhased='false'
$ResidencyMode='1'
$TerrainVertexReuse='false'
$TerrainResourcePhases='false'
$TerrainSplitVbo='false'
$ParallelActor='false'
$ParallelTerrainCpu='false'
$Experiment='P6-QUARANTINE-CONTROL'

switch($choice){
    '2'{$Experiment='P7-ACTOR-BATCH';$ParallelActor='true'}
    '3'{$Experiment='P7-TERRAIN-CPU';$ParallelTerrainCpu='true'}
    '4'{$Experiment='P7-ACTOR-TERRAIN';$ParallelActor='true';$ParallelTerrainCpu='true'}
    '5'{$Experiment='P7-ACTOR-TERRAIN-RESOURCES';$ParallelActor='true';$ParallelTerrainCpu='true';$TerrainResourcePhases='true'}
    '6'{$Experiment='P7-FULL-STUTTER';$ParallelActor='true';$ParallelTerrainCpu='true';$TerrainResourcePhases='true';$TerrainSplitVbo='true'}
}

New-Item -ItemType Directory -Path $ProfilesRoot -Force | Out-Null
$stamp=Get-Date -Format 'yyyyMMdd_HHmmss'
$ProfileDir=Join-Path $ProfilesRoot ("OptimizedMW_GL-P7_{0}_{1}" -f $Experiment,$stamp)
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
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw compile scheduler max queue age frames' '120'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw compile scheduler delete budget ms' '0.15'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw compile scheduler max objects per frame' '12'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw compile scheduler diagnostic threshold ms' '0.15'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw heavy compile lane mode' $HeavyMode
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw heavy compile threshold ms' '6.0'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw heavy compile min smooth frames' '30'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw heavy compile min headroom ms' '5.0'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw terrain drawable prior ms' '8.0'
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw terrain phased compile' $TerrainPhased
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw residency scheduler mode' $ResidencyMode
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw terrain immutable vertex reuse' $TerrainVertexReuse
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw terrain resource phases' $TerrainResourcePhases
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw terrain split vertex buffers' $TerrainSplitVbo
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw parallel actor binding' $ParallelActor
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw parallel terrain cpu prep' $ParallelTerrainCpu
    Set-IniValue $SettingsPath 'Cells' 'preload num threads' '1'

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
        "expected_lineage=optimizedmw/gl-p7-cpu-prep",
        "p3b_parallel_template_prefetch=true",
        "p3b_workers=1",
        "p3c_semantic_premerge=$P3C",
        "p3d_distant_display_lists=$P3D",
        "p4_compile_scheduler_mode=$SchedulerMode",
        "p4_max_budget_ms=2.0",
        "p4_credit_cap_ms=2.5",
        "p4_headroom_ratio=0.25",
        "p4_handoff_threshold_ms=20.0",
        "p4_max_queue_age_frames=120",
        "p4_delete_budget_ms=0.15",
        "p4_max_objects_per_frame=12",
        "p5_heavy_lane_mode=$HeavyMode",
        "p5_heavy_threshold_ms=6.0",
        "p5_heavy_min_smooth_frames=30",
        "p5_heavy_min_headroom_ms=5.0",
        "p5_terrain_drawable_prior_ms=8.0",
        "p5_terrain_phased_compile=$TerrainPhased",
        "p6_residency_scheduler_mode=$ResidencyMode",
        "p6_terrain_immutable_vertex_reuse=$TerrainVertexReuse",
        "p6_terrain_resource_phases=$TerrainResourcePhases",
        "p6_terrain_split_vertex_buffers=$TerrainSplitVbo",
        "p7_parallel_actor_binding=$ParallelActor",
        "p7_parallel_terrain_cpu_prep=$ParallelTerrainCpu",
        "preload_num_threads=1",
        "completion_governor=$Completion",
        "v38_compile_pacing_mode=3",
        "v315_adaptive_compile_governor=1",
        "diagnostic_render_handoff=true",
        "diagnostic_render_traversal_breakdown=true",
        "diagnostic_compile_ops=true",
        "openmw_exe_sha256=$exeHash",
        "settings_effective_sha256=$settingsHash",
        "openmw_cfg_sha256=$cfgHash"
    ) | Set-Content -LiteralPath (Join-Path $ProfileDir 'TEST_MODE.txt') -Encoding Ascii

    Remove-Item 'Env:OPENMW_V325_ACTOR_SOURCE_BATCH' -ErrorAction SilentlyContinue
    Remove-Item 'Env:OPENMW_V325_PARALLEL_ACTOR_BINDING' -ErrorAction SilentlyContinue

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
    $env:OPENMW_V3_FRAME_FILE=Join-Path $ProfileDir 'v3-frame.csv'
    $env:OPENMW_V3_HITCH_FILE=Join-Path $ProfileDir 'v3-hitch.csv'
    $env:OPENMW_P6_RENDER_PHASE_FILE=Join-Path $ProfileDir 'p6-render-phase.csv'
    $env:OPENMW_P6_TRAVERSAL_FILE=Join-Path $ProfileDir 'p6-render-traversal.csv'
    $env:OPENMW_V325_JOBGROUP_STATS_FILE=Join-Path $ProfileDir 'p7-actor-jobgroup.csv'
    $env:OPENMW_P7_PREP_STATS_FILE=Join-Path $ProfileDir 'p7-cpu-prep.csv'
    $env:OPENMW_OSG_STATS_FILE=Join-Path $ProfileDir 'v3-osg-stats.log'
    $env:OPENMW_OSG_STATS_LIST='times;resource'

    Write-Host ''
    Write-Host "Starting OptimizedMW P7 test: $Experiment" -ForegroundColor Green
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
    $exitCode=$process.ExitCode
    "exit_code=$exitCode" | Add-Content -LiteralPath (Join-Path $ProfileDir 'TEST_MODE.txt') -Encoding Ascii

    if($exitCode -ne 0){
        Get-ChildItem -LiteralPath $UserOpenMW -Filter 'openmw-crash*.dmp' -File -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTimeUtc -ge $start.UtcDateTime.AddSeconds(-5) } |
            ForEach-Object {
                Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $ProfileDir $_.Name) -Force -ErrorAction SilentlyContinue
            }
    }
}
finally{
    foreach($name in @(
        'OPENMW_V3_PAGING_FILE','OPENMW_V3_RENDER_FILE','OPENMW_V3_EVENT_FILE','OPENMW_V3_TRANSITION_FILE',
        'OPENMW_V3_RESOURCE_FILE','OPENMW_V3_STREAMING_FILE','OPENMW_V3_SHADOW_FILE','OPENMW_V36_BATCHING_FILE',
        'OPENMW_V32_GPU_MEMORY_FILE','OPENMW_P4_COMPILE_FILE','OPENMW_V3_FRAME_FILE','OPENMW_V3_HITCH_FILE',
        'OPENMW_P6_RENDER_PHASE_FILE','OPENMW_P6_TRAVERSAL_FILE','OPENMW_V325_JOBGROUP_STATS_FILE',
        'OPENMW_P7_PREP_STATS_FILE','OPENMW_OSG_STATS_FILE','OPENMW_OSG_STATS_LIST'
    )){Remove-Item ("Env:"+$name) -ErrorAction SilentlyContinue}

    if(Test-Path -LiteralPath (Join-Path $UserOpenMW 'openmw.log')){Copy-Item -LiteralPath (Join-Path $UserOpenMW 'openmw.log') -Destination (Join-Path $ProfileDir 'openmw.log') -Force -ErrorAction SilentlyContinue}
    if($changedSettings -and (Test-Path -LiteralPath $BackupSettings)){Copy-Item -LiteralPath $BackupSettings -Destination $SettingsPath -Force}
}

$zipPath="$ProfileDir.zip"
if(Test-Path -LiteralPath $zipPath){Remove-Item -LiteralPath $zipPath -Force}
Compress-Archive -Path (Join-Path $ProfileDir '*') -DestinationPath $zipPath -CompressionLevel Optimal
Write-Host ''
Write-Host 'P7 profile complete. Your normal settings have been restored.' -ForegroundColor Green
Write-Host "Upload this ZIP to ChatGPT: $zipPath" -ForegroundColor Cyan
try{Start-Process explorer.exe -ArgumentList "/select,`"$zipPath`""}catch{}
Read-Host 'Press Enter to close'
