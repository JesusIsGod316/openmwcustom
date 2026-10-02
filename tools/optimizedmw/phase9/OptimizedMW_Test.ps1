param(
    [Alias('Mode')][ValidateSet('','1','2','3','4','5','6','7','8','9','10','11','12','13')][string]$ModeChoice='',
    [ValidateSet('CONTROL','I1','I2','I1I2','D1')][string]$Candidate='CONTROL',
    [ValidateRange(-1,15)][int]$HitchMask=-1,
    [ValidateRange(-1,63)][int]$AttackMask=-1,
    [switch]$ObserverLocal,
    [ValidateSet('','Normal','Deep')][string]$TraceLevel='',
    [ValidateSet('stream','deferred')][string]$HitchTransport='stream',
    [switch]$SystemTrace,
    [ValidateRange(30,1800)][int]$SystemTraceDurationSeconds=600,
    [ValidateRange(0,33554432)][long]$BufferBudgetBytes=33554432,
    [string]$SavePath,[string]$RouteLabel='UNSPECIFIED',
    [ValidateSet('unclassified','fresh-process-first-visit','same-process-revisit','later-fresh-launch')][string]$Cohort='unclassified',
    [switch]$NoPrompt
)
$ErrorActionPreference = 'Stop'

$GameDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Exe = Join-Path $GameDir 'openmw.exe'
$UserOpenMW = Join-Path $env:USERPROFILE 'Documents\My Games\OpenMW'
$SettingsPath = Join-Path $UserOpenMW 'settings.cfg'
$OpenmwCfgPath = Join-Path $UserOpenMW 'openmw.cfg'
. (Join-Path $GameDir 'OptimizedMW_ProfileArchive.ps1')
. (Join-Path $GameDir 'OptimizedMW_HitchReport.ps1')
. (Join-Path $GameDir 'OptimizedMW_SystemTraceLink.ps1')
. (Join-Path $GameDir 'OptimizedMW_RunManifest.ps1')
. (Join-Path $GameDir 'OptimizedMW_HitchMatrix.ps1')
. (Join-Path $GameDir 'OptimizedMW_QuickTest.ps1')
. (Join-Path $GameDir 'OptimizedMW_AttackPresets.ps1')
$ProfilesRoot = Join-Path $UserOpenMW 'OptimizedMW-Phase9-Profiles'

if (-not (Test-Path -LiteralPath $Exe)) { Write-Host 'ERROR: Put this launcher beside openmw.exe.' -ForegroundColor Red; if(-not $NoPrompt){Read-Host 'Press Enter to close'}; exit 1 }
if (-not (Test-Path -LiteralPath $SettingsPath)) { Write-Host "ERROR: settings.cfg not found at $SettingsPath" -ForegroundColor Red; if(-not $NoPrompt){Read-Host 'Press Enter to close'}; exit 1 }

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

function Get-P9Mode {
    param([string]$Choice)
    # Identical P8U1 foundation. The driver-crashing static prewarm experiment
    # is intentionally unavailable in this continuation build.
    $result=@{Name='REFERENCE';Stream='0';Temporal='0';View='0';Trace='0';Ownership='0';Composite='0';DynamicMotion='0'}
    switch($Choice){
        '1'{}
        '2'{$result.Name='TEMPORAL-INPUTS';$result.Temporal='1'}
        '3'{$result.Name='ROOT-CAUSE-TRACE';$result.Trace='1'}
        '4'{$result.Name='TEMPORAL-TRACE';$result.Temporal='1';$result.Trace='1'}
        '5'{$result.Name='MOTION-VIEW';$result.Temporal='1';$result.View='1'}
        '6'{$result.Name='HITCH';$result.Stream='1'}
        '7'{$result.Name='HITCH-TRACE';$result.Stream='1';$result.Trace='1'}
        '8'{$result.Name='TEMPORAL-OWNERSHIP';$result.Temporal='1';$result.Ownership='1'}
        '9'{$result.Name='COMPOSITE-PREPARE';$result.Composite='1'}
        '10'{$result.Name='AUDITED-COMBINED';$result.Temporal='1';$result.Ownership='1';$result.Composite='1'}
        '11'{$result.Name='DYNAMIC-MOTION';$result.Temporal='1';$result.Ownership='1';$result.DynamicMotion='1'}
        '13'{$result.Name='OWNERSHIP-TRACE';$result.Temporal='1';$result.Ownership='1';$result.Trace='1'}
        '12'{$result.Name='AUDITED-TRACE';$result.Temporal='1';$result.Ownership='1';$result.Composite='1';$result.Trace='1'}
        default{throw 'Invalid Phase 9 mode'}
    }
    return $result
}

$quickSelection=$null
$quickSession=$null
$hitchSelection=$null
if(-not $ModeChoice -and $HitchMask -lt 0 -and $AttackMask -lt 0 -and -not $TraceLevel -and -not $NoPrompt){
    Write-Host ''
    Write-Host 'OptimizedMW - choose one test' -ForegroundColor Cyan
    Write-Host '  1 = Diagnose CONTROL   Deep engine diagnostics + automatic Windows trace' -ForegroundColor Green
    Write-Host '  2 = Normal CONTROL     performance comparison, original feature switches'
    Write-Host '  3 = Normal ALL         performance comparison, all four hitch candidates'
    Write-Host '  4 = Diagnose ALL       Deep engine diagnostics + automatic Windows trace'
    Write-Host '  5 = Normal ANIM control    original animation preparation'
    Write-Host '  6 = Normal ANIM retained   bounded animation ownership candidate' -ForegroundColor Green
    Write-Host '  7 = Diagnose ANIM retained Deep + automatic Windows trace'
    Write-Host '  C = Custom combination / I/O / trace depth'
    Write-Host '  A = Earlier Phase 9 modes'
    Write-Host '  X = Exit'
    do{$quickChoice=Read-Host 'Test (1-7, C, A, X; Enter = Normal CONTROL)';if($quickChoice -eq ''){$quickChoice='2'}}until($quickChoice -in @('1','2','3','4','5','6','7','C','A','X'))
    if($quickChoice -eq 'X'){exit 0}
    if($quickChoice -in @('1','2','3','4','5','6','7')){
        $quickSelection=Get-OptimizedMWQuickPreset -Choice $quickChoice
        $HitchMask=$quickSelection.mask;$TraceLevel=$quickSelection.trace_level
        $Candidate=$quickSelection.candidate;$SystemTrace=[bool]$quickSelection.system_trace
        if($quickSelection.PSObject.Properties['attack_mask']){$AttackMask=$quickSelection.attack_mask}
    }elseif($quickChoice -eq 'C'){
        $hitchSelection=Read-OptimizedMWHitchSelection
        $HitchMask=$hitchSelection.features.mask;$TraceLevel=if($hitchSelection.trace_level -eq 'DEEP'){'Deep'}else{'Normal'}
        $Candidate=$hitchSelection.candidate
        $AttackMask=Read-OptimizedMWAttackMask
        if($TraceLevel -eq 'Deep'){
            do{$systemChoice=Read-Host 'Windows system trace: Enter = automatic, N = engine only'}until($systemChoice -in @('','N'))
            $SystemTrace=$systemChoice -eq ''
        }
        $quickSelection=[pscustomobject]@{mask=$HitchMask;attack_mask=$AttackMask;trace_level=$TraceLevel;candidate=$Candidate;system_trace=[bool]$SystemTrace;name=$hitchSelection.experiment}
    }
}
if($HitchMask -ge 0 -or $AttackMask -ge 0 -or $TraceLevel){
    if($HitchMask -lt 0){$HitchMask=0}
    if(-not $TraceLevel){$TraceLevel='Normal'}
    $hitchSelection=Get-OptimizedMWHitchSelection -Mask $HitchMask -TraceLevel $TraceLevel -Candidate $Candidate
    if($ModeChoice -and $ModeChoice -ne $hitchSelection.phase9_mode){throw 'Hitch combinations use the reference foundation. Omit -Mode when selecting -HitchMask/-TraceLevel.'}
    $ModeChoice=$hitchSelection.phase9_mode
}
if($NoPrompt -and -not $ModeChoice){throw '-NoPrompt requires -Mode or -HitchMask/-TraceLevel'}
if(-not $ModeChoice){
Write-Host ''
Write-Host 'OptimizedMW - switchable hitch tests' -ForegroundColor Cyan
Write-Host '  H = CURRENT HITCH TESTS: all 16 combinations, Normal or Deep Trace' -ForegroundColor Green
Write-Host 'Previous Phase 9 tests:'
Write-Host '  1 = REFERENCE          unchanged P8U1 foundation'
Write-Host '  2 = TEMPORAL-INPUTS    camera/static motion + DLSS input-contract telemetry'
Write-Host '  3 = ROOT-CAUSE-TRACE   reference hitch diagnosis: objects/Lua/resources/state/GL'
Write-Host '  4 = TEMPORAL-TRACE     modes 2 and 3 together for one diagnostic run'
Write-Host '  5 = MOTION-VIEW        visualize generated motion vectors'
Write-Host '  8 = TEMPORAL-OWNERSHIP retained cull inputs (compare with mode 2)'
Write-Host '  9 = COMPOSITE-PREPARE terrain dependency preparation (compare with mode 1)'
Write-Host ' 10 = AUDITED-COMBINED   both candidates'
Write-Host ' 11 = DYNAMIC-MOTION     experimental moving geometry inputs'
Write-Host ' 12 = AUDITED-TRACE      combined candidate with deep diagnostics'
Write-Host ' 13 = OWNERSHIP-TRACE    mode 8 with hitch diagnostics; compare with mode 4'
Write-Host '  A = show old unpromoted HITCH1 diagnostic controls'
Write-Host 'Unsafe static prewarm is disabled. DLSS evaluation/jitter remain off until dense dynamic motion is valid.'
do{
    $choice=Read-Host 'Choose H for current tests, or a previous Phase 9 mode'
    if($choice -eq 'A'){
        Write-Host '  6 HITCH (old unpromoted refresh) | 7 HITCH-TRACE'
    }
    if($choice -eq 'H'){
        $hitchSelection=Read-OptimizedMWHitchSelection
        $Candidate=$hitchSelection.candidate
        $choice=$hitchSelection.phase9_mode
    }
}until($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13'))
}else{$choice=$ModeChoice}
$mode=Get-P9Mode $choice
if($ObserverLocal -and $mode.Trace -ne '1'){throw 'Local observer accounting requires Deep diagnostics'}
$attackCombination=Get-OptimizedMWAttackCombination $(if($AttackMask -ge 0){$AttackMask}else{0})
if($AttackMask -ge 0){
    $attackCombination=ConvertTo-OptimizedMWAttackSelection -Selection $hitchSelection -Mode $mode -Mask $AttackMask
}
if($SystemTrace -and $mode.Trace -ne '1'){throw 'Windows trace requires Deep diagnostics. Choose Diagnose CONTROL/ALL, or select Deep in Custom.'}
if($SystemTrace -and -not $quickSelection){
    $quickSelection=[pscustomobject]@{mask=$(if($HitchMask -ge 0){$HitchMask}else{0});trace_level='Deep';candidate=$Candidate;system_trace=$true;name=$mode.Name}
}
if($quickSelection -and $AttackMask -ge 0){
    $quickSelection.mask=$hitchSelection.features.mask
    $quickSelection.name=$hitchSelection.experiment
}
if($hitchSelection -and $AttackMask -lt 0){$mode.HitchTrialRevision=2}
$candidateProfile=Get-Phase9Candidate -Name $Candidate -BufferBudgetBytes $BufferBudgetBytes
$hitchCombination=if($hitchSelection){$hitchSelection.features}else{Get-OptimizedMWHitchCombination 0}
if($candidateProfile.d1 -and $mode.Trace -ne '1'){throw 'D1 requires a deep trace mode; use -Mode 3 -Candidate D1'}

$SchedulerMode='2'
$P3C='true'
$P3D='false'
$Completion='0'
$HeavyMode='0'
$TerrainPhased='false'
$ResidencyMode='1'
$TerrainVertexReuse='false'
$TerrainResourcePhases='true'
$TerrainSplitVbo='true'
$ParallelActor='true'
$ParallelTerrainCpu='true'
$OsgThreadingMode='0'
$Experiment='CONTROL'
$GroundcoverGpuPath='0'
$GroundcoverShadowReceive='true'
$GroundcoverPointLighting='true'

$Hierarchy='true'
$DensityLod='false'
$CullInputs='true'
$Lod2='false'
$ShadowBatch='false'
$CleanCapture='1'
$FrontToBack='false'
$FastWind='0'
$Experiment=$mode.Name+'-HITCHV1-'+$candidateProfile.name
if($hitchSelection){$Experiment=$hitchSelection.experiment}
$GpuDiagnostics=if($mode.Trace -eq '1' -or $mode.Temporal -eq '1'){'true'}else{'false'}
$ResourceRepair='true'
$LuaCache='true'
$SoundWarm='true'
$ShadowConsistency='true'

# Avoid running two launchers over the same settings file.
$existingGame=Get-Process openmw -ErrorAction SilentlyContinue
if($existingGame){throw 'Close OpenMW before starting a Phase 9 test.'}
$mutex=[Threading.Mutex]::new($false,'Local\OptimizedMW-Test-Profile')
$ownsMutex=$false
try{$ownsMutex=$mutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$ownsMutex=$true}
if(-not $ownsMutex){$mutex.Dispose();throw 'Another Phase 9 launcher is already running.'}
$originalEnv=Get-Phase9EnvironmentSnapshot

New-Item -ItemType Directory -Path $ProfilesRoot -Force | Out-Null
$stamp=Get-Date -Format 'yyyyMMdd_HHmmss_fff'
$ProfileDir=Join-Path $ProfilesRoot ("OptimizedMW_Phase9_{0}_{1}_{2}" -f $Experiment,$stamp,[Guid]::NewGuid().ToString('N').Substring(0,6))
New-Item -ItemType Directory -Path $ProfileDir -Force | Out-Null
$BackupSettings=Join-Path $ProfileDir 'settings-before.cfg'
Copy-Item -LiteralPath $SettingsPath -Destination $BackupSettings -Force
$changedSettings=$false
$launchFailure=$null
$restoreVerified=$true
$environmentRestoreVerified=$false
$runManifest=$null
$process=$null
$exitCode=$null
$start=[DateTimeOffset]::UtcNow
@("experiment=$Experiment","phase9_capture_state=initializing","dlss_runtime_available=false") |
    Set-Content -LiteralPath (Join-Path $ProfileDir 'TEST_MODE.txt') -Encoding Ascii
$beforeHash=(Get-FileHash -LiteralPath $BackupSettings -Algorithm SHA256).Hash

try{
    if($quickSelection){
        $quickSession=Start-OptimizedMWQuickSession -GameDirectory $GameDir -Selection $quickSelection -DurationSeconds $SystemTraceDurationSeconds
    }
    $changedSettings=$true

    # Clear inherited experimental/telemetry flags; restore the original environment afterward.
    Get-ChildItem Env: | Where-Object {$_.Name -like 'OPENMW_*'} | ForEach-Object {Remove-Item ('Env:'+$_.Name)}
    Set-Phase9CandidateEnvironment -Candidate $candidateProfile -Mode $mode
    Set-OptimizedMWHitchCombinationEnvironment $hitchCombination
    Set-OptimizedMWAttackEnvironment $attackCombination -ObserverLocal:$ObserverLocal

    # Same head-cache budget across every mode, not a hidden per-mode tuning change.
    Set-IniValue $SettingsPath 'Sound' 'head cache size' '96'
    Set-IniValue $SettingsPath 'Sound' 'warm sounds' $SoundWarm
    Set-IniValue $SettingsPath 'Lua' 'optimizedmw object userdata cache' $LuaCache
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw canonical terrain textures' $ResourceRepair
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw composite slicing' $ResourceRepair
    Set-IniValue $SettingsPath 'Shadows' 'optimizedmw setting consistency' $ShadowConsistency

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
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw shadow static batching' $ShadowBatch
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw active shadow batching' $ShadowBatch
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
    Set-IniValue $SettingsPath 'Cells' 'optimizedmw osg threading mode' $OsgThreadingMode
    Set-IniValue $SettingsPath 'Cells' 'preload num threads' '1'

    # Freeze the pre-existing compile policy so control and candidate differ only by P4.
    Set-IniValue $SettingsPath 'Cells' 'target framerate' '60'
    Set-IniValue $SettingsPath 'V3' 'v3.8 compile pacing mode' '3'
    Set-IniValue $SettingsPath 'V3' 'v3.15 adaptive compile governor' '1'
    Set-IniValue $SettingsPath 'V3' 'v3.6 async gpu profiler' $GpuDiagnostics
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
    Set-IniValue $SettingsPath 'Groundcover' 'optimizedmw gpu path' $GroundcoverGpuPath
    Set-IniValue $SettingsPath 'Groundcover' 'optimizedmw shadow receive' $GroundcoverShadowReceive
    Set-IniValue $SettingsPath 'Groundcover' 'point lighting' $GroundcoverPointLighting
    Set-IniValue $SettingsPath 'Groundcover' 'optimizedmw hierarchy' $Hierarchy
    Set-IniValue $SettingsPath 'Groundcover' 'optimizedmw density lod' $DensityLod
    Set-IniValue $SettingsPath 'Groundcover' 'optimizedmw cull input reuse' $CullInputs
    Set-IniValue $SettingsPath 'Groundcover' 'optimizedmw lod2' $Lod2
    Set-IniValue $SettingsPath 'Groundcover' 'optimizedmw front to back' $FrontToBack
    Set-IniValue $SettingsPath 'Groundcover' 'optimizedmw fast wind' $FastWind
    Set-IniValue $SettingsPath 'Groundcover' 'optimizedmw parallel preparation' 'true'
    Set-IniValue $SettingsPath 'Groundcover' 'optimizedmw minimum batch' '64'
    Set-IniValue $SettingsPath 'Groundcover' 'optimizedmw lod near' '3000'
    Set-IniValue $SettingsPath 'Groundcover' 'optimizedmw lod far' '10000'


    # Event 151 normal shadow cohort; no reduced resolution, distance, or cascade count.
    Set-IniValue $SettingsPath 'Shadows' 'number of shadow maps' '3'
    Set-IniValue $SettingsPath 'Shadows' 'shadow map resolution' '2048'
    Set-IniValue $SettingsPath 'Shadows' 'maximum shadow map distance' '4096'
    Set-IniValue $SettingsPath 'V3' 'v3.8 far shadow mode' '3'
    Set-IniValue $SettingsPath 'V3' 'v3.6 far caster minimum pixels' '5.0'

    if($mode.Trace -eq '1'){Set-IniValue $SettingsPath 'Cells' 'v3.2 renderer insertion profiling' 'true'}
    Copy-Item -LiteralPath $SettingsPath -Destination (Join-Path $ProfileDir 'settings-effective-test.cfg') -Force
    if(Test-Path -LiteralPath $OpenmwCfgPath){Copy-Item -LiteralPath $OpenmwCfgPath -Destination (Join-Path $ProfileDir 'openmw.cfg') -Force}
    if(Test-Path -LiteralPath (Join-Path $GameDir 'CI-ID.txt')){Copy-Item -LiteralPath (Join-Path $GameDir 'CI-ID.txt') -Destination (Join-Path $ProfileDir 'CI-ID.txt') -Force}

    $exeHash=(Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash
    $settingsHash=(Get-FileHash -LiteralPath $SettingsPath -Algorithm SHA256).Hash
    $cfgHash=if(Test-Path -LiteralPath $OpenmwCfgPath){(Get-FileHash -LiteralPath $OpenmwCfgPath -Algorithm SHA256).Hash}else{'MISSING'}

    @(
        "experiment=$Experiment",
        "hitch_trial_revision=$(if($AttackMask -ge 0){3}elseif($hitchSelection){2}else{1})",
        "hitch_attack_mask=$($attackCombination.mask)",
        "hitch_attack=$($attackCombination.name)",
        "hitch_animation_lifetime=$([int]$attackCombination.animation_lifetime)",
        "hitch_animation_plan=$([int]$attackCombination.animation_plan)",
        "hitch_composite_early=$([int]$attackCombination.composite_early)",
        "hitch_water_prepare=$([int]$attackCombination.water_prepare)",
        "hitch_weather_prepare=$([int]$attackCombination.weather_prepare)",
        "hitch_lua_package_descriptors=$([int]$attackCombination.lua_package_descriptors)",
        "hitch_observer_local=$([int][bool]$ObserverLocal)",
        "benchmark_context_schema=1",
        "hitch_diagnostics_revision=$(if($mode.Trace -eq '1'){1}else{0})",
        "hitch_combination_mask=$($hitchCombination.mask)",
        "hitch_combination=$($hitchCombination.name)",
        "hitch_trace_level=$(if($mode.Trace -eq '1'){'DEEP'}else{'NORMAL'})",
        "hitch_ui_reflow=$([int]$hitchCombination.ui_reflow)",
        "hitch_composite_batch=$([int]$hitchCombination.composite_batch)",
        "hitch_animation_preload=$([int]$hitchCombination.animation_preload)",
        "hitch_terrain_current_first=$([int]$hitchCombination.terrain_current_first)",
        "hitch_manifest_schema=1",
        "hitch_stream_policy_metrics_schema=1",
        "hitch_compile_attribution_schema=1",
        "hitch_file_policy_acknowledgement=1",
        "hitch_candidate=$($candidateProfile.name)",
        "hitch_i1_refill_bytes=$($candidateProfile.refill_bytes)",
        "hitch_i1_buffer_budget_bytes=$($candidateProfile.buffer_budget_bytes)",
        "hitch_i2_resident_seek=$([int]$candidateProfile.i2)",
        "hitch_d1_catalog_optimized=$([int]$candidateProfile.d1)",
        "hitch_compile_attribution=$($mode.Trace)",
        "hitch_activation_proof=runtime_policy_or_catalog_status_required_not_requested_flags",
        "expected_lineage=codex/optimizedmw-phase9-audited",
        "phase9_reference=P8U1_COMBINED_SAME_BINARY",
        "phase9_dynamic_stream=$($mode.Stream)",
        "phase9_temporal_inputs=$($mode.Temporal)",
        "phase9_temporal_ownership=$($mode.Ownership)",
        "phase9_composite_prepare=$($mode.Composite)",
        "phase9_dynamic_motion=$($mode.DynamicMotion)",
        "phase9_motion_view=$($mode.View)",
        "phase9_draw_trace=$($mode.Trace)",
        "phase9_hitch_spans=$($mode.Trace)",
        "phase9_hitch_scope=engine_operations_plus_selected_GL_not_all_driver_calls",
        "phase9_static_prewarm=disabled_after_optimized_trace_driver_crash",
        "phase9_trace_schema=6",
        "phase9_temporal_contract=consumer_frame_v2_ownership",
        "phase9_capture_transport=bounded_mpsc_v1",
        "phase9_hitch_transport_schema=$(if($mode.Trace -eq '1'){'1'}else{'0'})",
        "phase9_hitch_transport=$(if($mode.Trace -eq '1'){if($HitchTransport -eq 'stream'){'stream_mpsc_v1'}else{'deferred_arena_v1'}}else{'off'})",
        "hitch_diagnostics_revision=2",
        "hitch_observer_revision=1",
        "hitch_lifecycle_status=$($mode.Trace)",
        "phase9_resource_capacity=131072",
        "phase9_detail_revision=4",
        "phase9_hitch_capacity=524288",
        "phase9_hitch_schema=2",
        "phase9_dlss_ready_requires_dense_dynamic_motion=true",
        "phase9_gl_vulkan_interop_probe=$($mode.Temporal)",
        "phase9_culling=retained_existing_cell_paged_and_groundcover_hierarchy_no_new_visibility_policy",
        "phase9_dense_dynamic_motion=false",
        "phase9_scene_jitter=false",
        "phase9_performance_capture=$($mode.Trace -eq '0' -and $mode.View -eq '0')",
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
        "p8_hl1_osg_threading_mode=$OsgThreadingMode",
        "p8g_groundcover_gpu_path=$GroundcoverGpuPath",
        "p8g3_hierarchy=$Hierarchy",
        "p8g3_density_lod=$DensityLod",
        "p8g3_front_to_back=$FrontToBack",
        "p8g3_fast_wind=$FastWind",
        "p8g4_near=3000",
        "p8g4_far_requested=10000",
        "p8g4_far_effective=min(requested,0.9*groundcover_view_distance)",
        "p8g4_cull_inputs=$CullInputs",
        "p8u1_resource_repair=$ResourceRepair",
        "p8u1_lua_cache=$LuaCache",
        "p8u1_sound_warm=$SoundWarm",
        "p8u1_shadow_setting_consistency=$ShadowConsistency",
        "p8u1_head_cache_mib=96",
        "dlss_runtime_available=false",
        "p8u1_foundation=P8G4_CULL_CPU_PLUS_P8U1_COMBINED;head_cache_96MiB_common_to_all_modes",
        "p8g4_lod2=$Lod2",
        "p8g4_shadow_batch=$ShadowBatch",
        "p8g4_clean_capture=$CleanCapture",
        "p8g4_proxy_scope=opaque_static_near_middle;far_small_feature_falls_back",
        "p8g3_minimum_batch=64",
        "p8g3_stats=cumulative_instance_drawable_visits_not_unique_instances_or_gl_calls",
        "p8g_groundcover_shadow_receive=$GroundcoverShadowReceive",
        "p8g_groundcover_point_lighting=$GroundcoverPointLighting",
        "preload_num_threads=1",
        "completion_governor=$Completion",
        "v38_compile_pacing_mode=3",
        "v315_adaptive_compile_governor=1",
        "diagnostic_render_handoff=true",
        "diagnostic_render_traversal_breakdown=true",
        "diagnostic_dynamic_draw=false",
        "diagnostic_dynamic_deformation=false",
        "diagnostic_compile_ops=true",
        "openmw_exe_sha256=$exeHash",
        "settings_effective_sha256=$settingsHash",
        "openmw_cfg_sha256=$cfgHash"
    ) | Set-Content -LiteralPath (Join-Path $ProfileDir 'TEST_MODE.txt') -Encoding Ascii

    Remove-Item 'Env:OPENMW_V325_ACTOR_SOURCE_BATCH' -ErrorAction SilentlyContinue
    Remove-Item 'Env:OPENMW_V325_PARALLEL_ACTOR_BINDING' -ErrorAction SilentlyContinue

    # Focused nonblocking diagnostics. Avoid deep trace/profilers in performance runs.
    $env:OPENMW_P9_CAPTURE_TRANSPORT='1'
    $env:OPENMW_P9_RESOURCE_CAPACITY='131072'
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
    $env:OPENMW_P9_BENCHMARK_CONTEXT_FILE=Join-Path $ProfileDir 'benchmark-context.csv'
    $env:OPENMW_V3_HITCH_FILE=Join-Path $ProfileDir 'v3-hitch.csv'
    $env:OPENMW_P6_RENDER_PHASE_FILE=Join-Path $ProfileDir 'p6-render-phase.csv'
    $env:OPENMW_P6_TRAVERSAL_FILE=Join-Path $ProfileDir 'p6-render-traversal.csv'
    $env:OPENMW_V325_JOBGROUP_STATS_FILE=Join-Path $ProfileDir 'p7-actor-jobgroup.csv'
    $env:OPENMW_P7_PREP_STATS_FILE=Join-Path $ProfileDir 'p7-cpu-prep.csv'
    if($CleanCapture -eq '0'){$env:OPENMW_OSG_STATS_FILE=Join-Path $ProfileDir 'v3-osg-stats.log'}
    $env:OPENMW_P8G4_CLEAN_CAPTURE=$CleanCapture
    $env:OPENMW_P8G4_CAPTURE_DIR=$ProfileDir
    $env:OPENMW_P8G4_STATS='1'
    $env:OPENMW_OSG_STATS_LIST='times;resource'
    $env:OPENMW_P8G3_STATS='1'
    Remove-Item 'Env:OPENMW_P9_STATIC_PREWARM' -ErrorAction SilentlyContinue
    Remove-Item 'Env:OPENMW_P9_STATIC_PREWARM_FILE' -ErrorAction SilentlyContinue
    $env:OPENMW_P9_DYNAMIC_STREAM=$mode.Stream
    $env:OPENMW_P9_TEMPORAL_INPUTS=$mode.Temporal
    $env:OPENMW_P9_TEMPORAL_OWNERSHIP=$mode.Ownership
    $env:OPENMW_P9_COMPOSITE_PREPARE=$mode.Composite
    $env:OPENMW_P9_DYNAMIC_MOTION=$mode.DynamicMotion
    $env:OPENMW_P9_MOTION_VIEW=$mode.View
    if($mode.Temporal -eq '1'){
        $env:OPENMW_P9_TEMPORAL_FILE=Join-Path $ProfileDir 'p9-temporal-inputs.csv'
        $env:OPENMW_P9_DLSS_CAPS_FILE=Join-Path $ProfileDir 'p9-dlss-capabilities.csv'
    }
    if($mode.Trace -eq '1'){
        $env:OPENMW_P9_COMPILE_ATTRIBUTION_FILE=Join-Path $ProfileDir 'compile-attribution.csv'
        Set-Phase9HitchRecorderEnvironment -Mode $mode -Transport $HitchTransport
        $env:OPENMW_P9_HITCH_CAPACITY='524288'
        $env:OPENMW_P9_HITCH_SPANS_FILE=Join-Path $ProfileDir 'p9-hitch-spans.csv'
        $env:OPENMW_V3_INSERT_FILE=Join-Path $ProfileDir 'v3-insertion.csv'
        $env:OPENMW_V32_RENDER_INSERT_FILE=Join-Path $ProfileDir 'v32-render-insertion.csv'
        $env:OPENMW_V3_LUASYNC_FILE=Join-Path $ProfileDir 'v3-lua-sync.csv'
        $env:OPENMW_V3_LUA_ACTION_FILE=Join-Path $ProfileDir 'v3-lua-actions.csv'
        $env:OPENMW_V33_LUA_CALLBACK_FILE=Join-Path $ProfileDir 'v33-lua-callbacks.csv'
        $env:OPENMW_V35_LUA_LOAD_FILE=Join-Path $ProfileDir 'v35-lua-load.csv'
        $env:OPENMW_V3_LUA_UPDATE_FILE=Join-Path $ProfileDir 'v3-lua-update.csv'
        $env:OPENMW_V36_CONTROLLER_FILE=Join-Path $ProfileDir 'v36-controller.csv'
        $env:OPENMW_V36_LUA_ADDSCRIPT_FILE=Join-Path $ProfileDir 'v36-lua-addscript.csv'
        $env:OPENMW_P9_LEAF_TRACE_FILE=Join-Path $ProfileDir 'p9-draw-phases.csv'
        $env:OPENMW_P9_DYNAMIC_TRACE_FILE=Join-Path $ProfileDir 'p9-dynamic-stream.csv'
        $env:OPENMW_P9_COMPOSITE_FILE=Join-Path $ProfileDir 'p9-terrain-composite.csv'
    }
    if($mode.Trace -eq '1' -or $mode.Temporal -eq '1'){
        $env:OPENMW_V36_GPU_PASS_FILE=Join-Path $ProfileDir 'p9-gpu-passes.csv'
    }
    $runManifest=New-Phase9RunManifest -GameDir $GameDir -ProfileDir $ProfileDir -Mode $mode -Candidate $candidateProfile `
        -SettingsPath $SettingsPath -OpenmwCfgPath $OpenmwCfgPath -SavePath $SavePath -RouteLabel $RouteLabel -Cohort $Cohort -HitchCombination $hitchCombination
    Save-Phase9RunManifest $runManifest $ProfileDir

    Write-Host ''
    Write-Host "Starting OptimizedMW Phase 9 test: $Experiment" -ForegroundColor Green
    Write-Host 'Use the SAME outdoor save, fixed heavy view, walking route, and frame-cap state.' -ForegroundColor Yellow
    if($mode.Trace -eq '1'){Write-Host 'Trace modes are diagnostic; do not use their FPS as a promotion result.' -ForegroundColor Yellow}
    Write-Host 'Hold the fixed view ~45 sec, then walk the same 2-3 minute route across the same cell boundaries. Quit normally.'
    Write-Host ''

    $memoryCsv=Join-Path $ProfileDir 'process-memory.csv'
    $memoryLines=[System.Collections.Generic.List[string]]::new(8192)
    [void]$memoryLines.Add('epoch_ms,elapsed_s,working_set_mb,private_mb,virtual_mb,cpu_total_s')
    $memoryDropped=0
    $start=[DateTimeOffset]::UtcNow
    $systemTraceLink=New-Phase9SystemTraceLink -GameDir $GameDir -ProfileDir $ProfileDir -Executable $Exe -ExecutableSha256 $exeHash
    if($SystemTrace -and (-not $systemTraceLink -or $systemTraceLink.capture_id -ne $quickSession.trace_state.capture_id)){
        throw 'The automatic Windows recorder did not link to this exact test. The game was not started.'
    }
    $process=Start-Process -FilePath $Exe -WorkingDirectory $GameDir -PassThru
    $gameProcessId=$process.Id
    $gameCreationUtc=$process.StartTime.ToUniversalTime().ToString('o')
    $runManifest.process.pid=$gameProcessId
    $runManifest.process.creation_utc=$gameCreationUtc
    $runManifest.process.creation_ticks=$process.StartTime.ToUniversalTime().Ticks.ToString([Globalization.CultureInfo]::InvariantCulture)
    $runManifest.process.launch_utc=$start.ToString('o')
    $runManifest.completion.state='running'
    Save-Phase9RunManifest $runManifest $ProfileDir
    @("game_process_id=$gameProcessId","game_creation_utc=$gameCreationUtc") | Add-Content -LiteralPath (Join-Path $ProfileDir 'TEST_MODE.txt') -Encoding Ascii
    while(-not $process.HasExited){
        try{
            $process.Refresh()
            $now=[DateTimeOffset]::UtcNow
            $line='{0},{1:F3},{2:F1},{3:F1},{4:F1},{5:F3}' -f $now.ToUnixTimeMilliseconds(),($now-$start).TotalSeconds,($process.WorkingSet64/1MB),($process.PrivateMemorySize64/1MB),($process.VirtualMemorySize64/1MB),$process.TotalProcessorTime.TotalSeconds
            if($memoryLines.Count -lt 8192){[void]$memoryLines.Add($line)}else{$memoryDropped++}
        }catch{}
        Start-Sleep -Milliseconds 1000
    }
    $process.WaitForExit()
    $exitCode=$process.ExitCode
    [IO.File]::WriteAllLines($memoryCsv,$memoryLines,[Text.Encoding]::ASCII)
    "memory_samples_dropped=$memoryDropped" | Add-Content -LiteralPath (Join-Path $ProfileDir 'TEST_MODE.txt') -Encoding Ascii
    "exit_code=$exitCode" | Add-Content -LiteralPath (Join-Path $ProfileDir 'TEST_MODE.txt') -Encoding Ascii

    if($exitCode -ne 0){
        Get-ChildItem -LiteralPath $UserOpenMW -Filter 'openmw-crash*.dmp' -File -ErrorAction SilentlyContinue |
            Where-Object { $_.LastWriteTimeUtc -ge $start.UtcDateTime.AddSeconds(-5) } |
            ForEach-Object {
                Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $ProfileDir $_.Name) -Force -ErrorAction SilentlyContinue
            }
    }
}
catch{
    $launchFailure=$_
    "Launcher failure: $_" | Set-Content -LiteralPath (Join-Path $ProfileDir 'LAUNCH-ERROR.txt') -Encoding UTF8
    Write-Host "Launcher failed: $_" -ForegroundColor Red
    # A failure before Start-Process still needs a package/configuration identity.
    if(-not $runManifest){
        try{
            $runManifest=New-Phase9RunManifest -GameDir $GameDir -ProfileDir $ProfileDir -Mode $mode -Candidate $candidateProfile `
                -SettingsPath $SettingsPath -OpenmwCfgPath $OpenmwCfgPath -SavePath $SavePath -RouteLabel $RouteLabel -Cohort $Cohort -HitchCombination $hitchCombination
        }catch{"Run manifest preparation failed: $_" | Set-Content -LiteralPath (Join-Path $ProfileDir 'MANIFEST-ERROR.txt') -Encoding UTF8}
    }
}
finally{
    # Never change the shared settings back while the launched game still uses them.
    if($null -ne $process){
        try{if(-not $process.HasExited){$process.WaitForExit()};$exitCode=$process.ExitCode}catch{}
    }
    if($quickSession){
        try{Stop-OptimizedMWQuickSession -Context $quickSession}catch{Write-Warning ('Recorder stop request failed: '+$_)}
    }
    try{
        $environmentRestoreVerified=Restore-Phase9Environment $originalEnv
        if(-not $environmentRestoreVerified){throw 'Environment read-back differs from original'}
    }catch{Write-Host "Environment restore failed: $_" -ForegroundColor Red}
    try{
        if($changedSettings){
            Copy-Item -LiteralPath $BackupSettings -Destination $SettingsPath -Force
            $restoreVerified=((Get-FileHash -LiteralPath $SettingsPath -Algorithm SHA256).Hash -eq $beforeHash)
        }
    }catch{$restoreVerified=$false}
    if(-not $restoreVerified){Write-Host "RESTORE FAILED: restore $BackupSettings before playing." -ForegroundColor Red}
    try{
        $log=Join-Path $UserOpenMW 'openmw.log'
        if((Test-Path -LiteralPath $log) -and (Get-Item -LiteralPath $log).LastWriteTimeUtc -ge $start.UtcDateTime.AddSeconds(-5)){
            Copy-Item -LiteralPath $log -Destination (Join-Path $ProfileDir 'openmw.log') -Force
        }
        @("settings_restore_verified=$restoreVerified","environment_restore_verified=$environmentRestoreVerified","phase9_capture_state=finished", "final_exit_code=$exitCode") |
            Add-Content -LiteralPath (Join-Path $ProfileDir 'TEST_MODE.txt') -Encoding Ascii
        if($runManifest){
            $runManifest.completion.state=if($launchFailure){'launcher_failed'}else{'game_exited'}
            $runManifest.completion.exit_code=$exitCode
            $runManifest.completion.launcher_error=if($launchFailure){[string]$launchFailure}else{$null}
            $runManifest.completion.settings_restore_verified=$restoreVerified
            $runManifest.completion.environment_restore_verified=$environmentRestoreVerified
            $runManifest.completion.restored_settings_sha256=(Get-Phase9FileIdentity $SettingsPath).sha256
            Save-Phase9RunManifest $runManifest $ProfileDir
        }
    }catch{Write-Host "Some final evidence could not be copied: $_" -ForegroundColor Yellow}
    try{if($ownsMutex){$mutex.ReleaseMutex()}}finally{$mutex.Dispose()}
}

try{
    $traceStatus=Test-Phase9TraceCapture -ProfileDir $ProfileDir -TraceRequested ($mode.Trace -eq '1')
    $temporalStatus=Test-Phase9TemporalCapture -ProfileDir $ProfileDir -TemporalRequested ($mode.Temporal -eq '1')
    $writerStatus=Test-Phase9WriterCapture -ProfileDir $ProfileDir
    $hitchStatus=Test-Phase9HitchCapture -ProfileDir $ProfileDir
    $diagnostics6Status=Test-Phase9Diagnostics6Capture -ProfileDir $ProfileDir
    $compileAttributionStatus=Test-Phase9CompileAttributionCapture -ProfileDir $ProfileDir
    if($runManifest){Complete-Phase9RunManifest $runManifest $ProfileDir}
    $zip=Complete-Phase9Profile -ProfileDir $ProfileDir -GameDir $GameDir
    if($systemTraceLink){$null=Complete-Phase9SystemTraceLink -Link $systemTraceLink -ProfileZip $zip.Path -GameProcessId $gameProcessId -GameCreationUtc $gameCreationUtc -DeferQualification:([bool]$quickSession)}
    $uploadPath=$zip.Path
    if($quickSession){
        $quickResult=Complete-OptimizedMWQuickSession -Context $quickSession -ProfileZip $zip.Path -Link $systemTraceLink `
            -GameProcessId $gameProcessId -GameCreationUtc $gameCreationUtc -RestoreVerified $restoreVerified `
            -EnvironmentRestoreVerified $environmentRestoreVerified -LauncherError $(if($launchFailure){[string]$launchFailure}elseif($exitCode -ne 0){"Game exited with code $exitCode"}else{''})
        if($quickResult.upload_path){$uploadPath=$quickResult.upload_path}
    }
    Write-Host ''
    if($restoreVerified){Write-Host 'Your normal settings have been restored.' -ForegroundColor Green}
    Write-Host "Upload this ZIP: $uploadPath" -ForegroundColor Cyan
    if($zip.Missing.Count){Write-Host ('Partial capture; missing: '+($zip.Missing -join ', ')) -ForegroundColor Yellow}
    if(-not $NoPrompt){try{Start-Process explorer.exe -ArgumentList "/select,`"$uploadPath`""}catch{}}
}catch{
    Write-Host "$_" -ForegroundColor Red
    Write-Host "Raw evidence remains at: $ProfileDir" -ForegroundColor Yellow
    if(-not $NoPrompt){Read-Host 'Press Enter to close'}
    exit 2
}
if(-not $NoPrompt){Read-Host 'Press Enter to close'}
if(-not $restoreVerified -or -not $environmentRestoreVerified -or $null -ne $launchFailure){exit 1}
