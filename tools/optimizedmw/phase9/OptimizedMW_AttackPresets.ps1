# New mechanisms are independent of the original four-bit HITCHV2 vector.
# The old preset names and meanings remain available unchanged.
function Get-OptimizedMWAttackCombination {
    param([ValidateRange(0,63)][int]$Mask=0)
    $labels=[Collections.Generic.List[string]]::new()
    foreach($entry in @(@(1,'ANIM-RETAIN'),@(2,'ANIM-PLAN'),@(4,'EARLY-BAKE'),
        @(8,'WATER'),@(16,'WEATHER'),@(32,'LUA-PACKAGES'))){
        if($Mask -band $entry[0]){$labels.Add([string]$entry[1])}
    }
    return [pscustomobject][ordered]@{schema=1;mask=$Mask;
        name=$(if($labels.Count){$labels -join '+'}else{'CONTROL'});
        animation_lifetime=[bool]($Mask -band 1);animation_plan=[bool]($Mask -band 2);
        composite_early=[bool]($Mask -band 4);water_prepare=[bool]($Mask -band 8);
        weather_prepare=[bool]($Mask -band 16);lua_package_descriptors=[bool]($Mask -band 32)}
}

function Set-OptimizedMWAttackEnvironment {
    param([Parameter(Mandatory=$true)][object]$Combination,[switch]$ObserverLocal)
    $env:OPENMW_HITCH_ANIMATION_LIFETIME=[string][int]$Combination.animation_lifetime
    $env:OPENMW_HITCH_ANIMATION_PLAN=[string][int]$Combination.animation_plan
    $env:OPENMW_HITCH_COMPOSITE_EARLY=[string][int]$Combination.composite_early
    $env:OPENMW_HITCH_WATER_PREPARE=[string][int]$Combination.water_prepare
    $env:OPENMW_HITCH_WEATHER_PREPARE=[string][int]$Combination.weather_prepare
    $env:OPENMW_HITCH_LUA_PACKAGE_DESCRIPTORS=[string][int]$Combination.lua_package_descriptors
    $env:OPENMW_P9_OBSERVER_LOCAL=$(if($ObserverLocal){'1'}else{'0'})
    if($Combination.animation_lifetime){$env:OPENMW_HITCH_ANIMATION_PRELOAD='1'}
    if($Combination.composite_early){$env:OPENMW_P9_COMPOSITE_PREPARE='1'}
}

function ConvertTo-OptimizedMWAttackSelection {
    param([Parameter(Mandatory=$true)][object]$Selection,[Parameter(Mandatory=$true)][object]$Mode,
        [ValidateRange(0,63)][int]$Mask)
    $combination=Get-OptimizedMWAttackCombination $Mask
    $features=Get-OptimizedMWHitchCombination $(if($combination.animation_lifetime){$Selection.features.mask -bor 4}else{$Selection.features.mask})
    $features | Add-Member -NotePropertyName attack -NotePropertyValue $combination
    $Selection.features=$features
    $Selection.experiment=('HITCHV3-{0}-A{1}-{2}-IO-{3}' -f $features.name,$Mask,$Selection.trace_level,$Selection.candidate)
    $Mode.HitchTrialRevision=3
    if($combination.composite_early){$Mode.Composite='1'}
    return $combination
}

function Read-OptimizedMWAttackMask {
    Write-Host 'New audited mechanisms (add values to combine):'
    Write-Host '  1 ANIM-RETAIN | 2 ANIM-PLAN | 4 EARLY-BAKE | 8 WATER | 16 WEATHER | 32 LUA-PACKAGES'
    Write-Host 'EARLY-BAKE explicitly enables the separate P9 composite preparation path.'
    Write-Host 'Start with one mechanism; combined performance is not established.'
    do{$text=Read-Host 'New mechanism mask (Enter = 0, original behavior)'}until($text -eq '' -or $text -match '^(?:[0-9]|[1-5][0-9]|6[0-3])$')
    return $(if($text -eq ''){0}else{[int]$text})
}
