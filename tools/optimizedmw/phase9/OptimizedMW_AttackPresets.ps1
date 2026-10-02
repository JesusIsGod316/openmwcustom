# New mechanisms are independent of the original four-bit HITCHV2 vector.
# The old preset names and meanings remain available unchanged.
function Get-OptimizedMWAttackCombination {
    param([ValidateRange(0,511)][int]$Mask=0)
    if(($Mask -band 128) -and -not ($Mask -band 1)){throw 'WARM-KF requires ANIM-RETAIN (add bit 1)'}
    $labels=[Collections.Generic.List[string]]::new()
    foreach($entry in @(@(1,'ANIM-RETAIN'),@(2,'ANIM-PLAN'),@(4,'EARLY-BAKE'),
        @(8,'WATER'),@(16,'WEATHER'),@(32,'LUA-PACKAGES'),@(64,'DELETE-ADMIT'),@(128,'WARM-KF'),@(256,'STEP'))){
        if($Mask -band $entry[0]){$labels.Add([string]$entry[1])}
    }
    return [pscustomobject][ordered]@{schema=1;mask=$Mask;
        name=$(if($labels.Count){$labels -join '+'}else{'CONTROL'});
        animation_lifetime=[bool]($Mask -band 1);animation_plan=[bool]($Mask -band 2);
        composite_early=[bool]($Mask -band 4);water_prepare=[bool]($Mask -band 8);
        weather_prepare=[bool]($Mask -band 16);lua_package_descriptors=[bool]($Mask -band 32);
        delete_admission=[bool]($Mask -band 64);animation_warm_lease=[bool]($Mask -band 128);
        composite_step=[bool]($Mask -band 256)}
}

function Set-OptimizedMWAttackEnvironment {
    param([Parameter(Mandatory=$true)][object]$Combination,[switch]$ObserverLocal,
        [switch]$CompositeProgress,[switch]$CompositeCpuMips)
    $env:OPENMW_HITCH_ANIMATION_LIFETIME=[string][int]$Combination.animation_lifetime
    $env:OPENMW_HITCH_ANIMATION_PLAN=[string][int]$Combination.animation_plan
    $env:OPENMW_HITCH_COMPOSITE_EARLY=[string][int]$Combination.composite_early
    $env:OPENMW_HITCH_WATER_PREPARE=[string][int]$Combination.water_prepare
    $env:OPENMW_HITCH_WEATHER_PREPARE=[string][int]$Combination.weather_prepare
    $env:OPENMW_HITCH_LUA_PACKAGE_DESCRIPTORS=[string][int]$Combination.lua_package_descriptors
    $env:OPENMW_P9_DELETE_ADMISSION=[string][int]$Combination.delete_admission
    $env:OPENMW_HITCH_ANIMATION_WARM_LEASE=[string][int]$Combination.animation_warm_lease
    $env:OPENMW_HITCH_COMPOSITE_STEP=[string][int]$Combination.composite_step
    $env:OPENMW_HITCH_COMPOSITE_PROGRESS=[string][int][bool]$CompositeProgress
    $env:OPENMW_HITCH_COMPOSITE_CPU_MIPS=[string][int][bool]$CompositeCpuMips
    $env:OPENMW_P9_OBSERVER_LOCAL=$(if($ObserverLocal){'1'}else{'0'})
    if($Combination.animation_lifetime){$env:OPENMW_HITCH_ANIMATION_PRELOAD='1'}
    if($Combination.composite_early -or $Combination.composite_step){$env:OPENMW_P9_COMPOSITE_PREPARE='1'}
}

# Readable presets keep the historic attack vector intact. The new mechanisms
# have their own explicit fields instead of silently acquiring old mask bits.
function Get-OptimizedMWRendererPreset {
    param([ValidateSet('CONTROL','CLEANUP','STEP','EARLY-STEP','TERRAIN-MIPS','COMBINED','COMBINED-MIPS')][string]$Name='CONTROL',
        [ValidateSet('Normal','Deep')][string]$TraceLevel='Normal',[switch]$SystemTrace)
    if($SystemTrace -and $TraceLevel -ne 'Deep'){throw 'Windows recording requires Deep; Normal renderer tests never start WPR'}
    $presetName=$Name.ToUpperInvariant()
    $mask=switch($presetName){'CONTROL'{1};'CLEANUP'{65};'STEP'{257};'EARLY-STEP'{261};'TERRAIN-MIPS'{261};'COMBINED'{325};'COMBINED-MIPS'{325}}
    return [pscustomobject][ordered]@{mask=4;attack_mask=$mask;trace_level=$TraceLevel;
        candidate='CONTROL';system_trace=[bool]$SystemTrace;gpu_query_defer=($TraceLevel -eq 'Deep');
        focused_preset=$presetName;composite_progress=($presetName -in @('EARLY-STEP','TERRAIN-MIPS','COMBINED','COMBINED-MIPS'));
        composite_cpu_mips=($presetName -in @('TERRAIN-MIPS','COMBINED-MIPS'));name=('RENDER1-'+$presetName)}
}

function Read-OptimizedMWRendererPreset {
    Write-Host 'Focused renderer tests (warm keyframes, weather and other candidates stay off):' -ForegroundColor Cyan
    Write-Host '  1 = Retained CONTROL | 2 = Cleanup only | 3 = Original STEP'
    Write-Host '  4 = Early STEP + progress repair | 5 = Terrain + supplied blendmap mips'
    Write-Host '  6 = Combined renderer (early STEP + progress + cleanup, mips off)'
    Write-Host 'First comparison: F/1, F/5, F/5, F/1, each in Normal mode.' -ForegroundColor Yellow
    do{$choice=Read-Host 'Renderer preset (1-6; Enter = 1)';if($choice -eq ''){$choice='1'}}until($choice -in @('1','2','3','4','5','6'))
    do{$depth=Read-Host 'N = Normal performance | D = Deep engine diagnostics (Enter = N)';if($depth -eq ''){$depth='N'}}until($depth -in @('N','D'))
    $system=$false
    if($depth -eq 'D'){
        do{$windows=Read-Host 'Windows recording: Enter = engine only, W = include Windows'}until($windows -in @('','W'))
        $system=$windows -eq 'W'
    }
    $names=@('CONTROL','CLEANUP','STEP','EARLY-STEP','TERRAIN-MIPS','COMBINED')
    $name=$names[[int]$choice-1]
    if($choice -eq '6'){
        do{$mips=Read-Host 'Combined supplied mips: Enter = off, M = on'}until($mips -in @('','M'))
        if($mips -eq 'M'){$name='COMBINED-MIPS'}
    }
    return Get-OptimizedMWRendererPreset $name $(if($depth -eq 'D'){'Deep'}else{'Normal'}) -SystemTrace:$system
}

function Set-OptimizedMWRendererIdentity {
    param([Parameter(Mandatory=$true)][object]$Selection,[Parameter(Mandatory=$true)][object]$Mode,
        [string]$Preset='CUSTOM',[switch]$CompositeProgress,[switch]$CompositeCpuMips)
    $identity=[pscustomobject][ordered]@{revision=1;preset=$Preset;
        composite_progress=[bool]$CompositeProgress;composite_cpu_mips=[bool]$CompositeCpuMips}
    $Selection.features | Add-Member -NotePropertyName renderer -NotePropertyValue $identity
    if($Preset -ne 'CUSTOM'){
        $Selection.experiment=('RENDER1-{0}-{1}-IO-{2}' -f $Preset,$Selection.trace_level,$Selection.candidate)
        $Mode | Add-Member -NotePropertyName FocusedRendererTrialRevision -NotePropertyValue 1
    }else{
        $Selection.experiment+=('-PROGRESS{0}-MIPS{1}' -f [int][bool]$CompositeProgress,[int][bool]$CompositeCpuMips)
    }
    return $identity
}

function ConvertTo-OptimizedMWAttackSelection {
    param([Parameter(Mandatory=$true)][object]$Selection,[Parameter(Mandatory=$true)][object]$Mode,
        [ValidateRange(0,511)][int]$Mask)
    $combination=Get-OptimizedMWAttackCombination $Mask
    $features=Get-OptimizedMWHitchCombination $(if($combination.animation_lifetime){$Selection.features.mask -bor 4}else{$Selection.features.mask})
    $features | Add-Member -NotePropertyName attack -NotePropertyValue $combination
    $Selection.features=$features
    $Selection.experiment=('HITCHV3-{0}-A{1}-{2}-IO-{3}' -f $features.name,$Mask,$Selection.trace_level,$Selection.candidate)
    $Mode.HitchTrialRevision=3
    if($combination.composite_early -or $combination.composite_step){$Mode.Composite='1'}
    return $combination
}

function Read-OptimizedMWAttackMask {
    Write-Host 'New audited mechanisms (add values to combine):'
    Write-Host '  1 ANIM-RETAIN | 2 ANIM-PLAN | 4 EARLY-BAKE | 8 WATER | 16 WEATHER | 32 LUA-PACKAGES'
    Write-Host ' 64 DELETE-ADMIT | 128 WARM-KF (requires 1) | 256 STEP'
    Write-Host 'EARLY-BAKE and STEP explicitly enable P9 composite preparation; STEP does not enable EARLY-BAKE.'
    Write-Host 'Start with one mechanism; combined performance is not established.'
    do{$text=Read-Host 'New mechanism mask (0-511; Enter = 0, original behavior)'}until($text -eq '' -or
        ($text -match '^\d{1,3}$' -and [int]$text -le 511 -and (-not ([int]$text -band 128) -or ([int]$text -band 1))))
    return $(if($text -eq ''){0}else{[int]$text})
}
