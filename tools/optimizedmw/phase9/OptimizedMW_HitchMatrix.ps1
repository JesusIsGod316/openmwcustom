# Production launcher selection. Trace depth is independent of gameplay features.
function Get-OptimizedMWHitchCombination {
    param([ValidateRange(0,15)][int]$Mask=0)
    $labels=[Collections.Generic.List[string]]::new()
    if($Mask -band 1){$labels.Add('UI')}
    if($Mask -band 2){$labels.Add('BAKE')}
    if($Mask -band 4){$labels.Add('ANIM')}
    if($Mask -band 8){$labels.Add('TERRAIN')}
    return [pscustomobject][ordered]@{schema=1;mask=$Mask;
        name=$(if($labels.Count){$labels -join '+'}else{'CONTROL'});
        ui_reflow=[bool]($Mask -band 1);composite_batch=[bool]($Mask -band 2);
        animation_preload=[bool]($Mask -band 4);terrain_current_first=[bool]($Mask -band 8)}
}

function Set-OptimizedMWHitchCombinationEnvironment {
    param([Parameter(Mandatory=$true)][object]$Combination)
    $env:OPENMW_HITCH_UI_REFLOW=if($Combination.ui_reflow){'1'}else{'0'}
    $env:OPENMW_HITCH_COMPOSITE_BATCH=if($Combination.composite_batch){'1'}else{'0'}
    $env:OPENMW_HITCH_ANIMATION_PRELOAD=if($Combination.animation_preload){'1'}else{'0'}
    $env:OPENMW_HITCH_TERRAIN_CURRENT_FIRST=if($Combination.terrain_current_first){'1'}else{'0'}
}

function Get-OptimizedMWHitchSelection {
    param([ValidateRange(0,15)][int]$Mask=0,
        [ValidateSet('Normal','Deep')][string]$TraceLevel='Normal',
        [ValidateSet('CONTROL','I1','I2','I1I2','D1')][string]$Candidate='CONTROL')
    if($Candidate -eq 'D1' -and $TraceLevel -ne 'Deep'){throw 'D1 is a diagnostic collector; select Deep Trace for D1.'}
    $features=Get-OptimizedMWHitchCombination $Mask
    $depth=if($TraceLevel -eq 'Deep'){'DEEP'}else{'NORMAL'}
    return [pscustomobject][ordered]@{features=$features;trace_level=$depth;candidate=$Candidate.ToUpperInvariant();
        phase9_mode=$(if($TraceLevel -eq 'Deep'){'3'}else{'1'});
        experiment=('HITCHV2-{0}-{1}-IO-{2}' -f $features.name,$depth,$Candidate.ToUpperInvariant())}
}

function Read-OptimizedMWHitchSelection {
    Write-Host ''
    Write-Host 'OptimizedMW hitch tests - choose features, then trace depth' -ForegroundColor Cyan
    Write-Host 'UI = text/layout reuse | BAKE = terrain layer state batching'
    Write-Host 'ANIM = NPC animation preparation | TERRAIN = demanded-grid readiness'
    foreach($mask in 0..15){
        $combination=Get-OptimizedMWHitchCombination $mask
        Write-Host (' {0,2} = {1}' -f $mask,$combination.name)
    }
    do{$maskText=Read-Host 'Combination (0 = CONTROL, 15 = ALL)'}until($maskText -match '^(?:[0-9]|1[0-5])$')
    do{$depthText=Read-Host 'N = Normal performance capture | D = Deep Trace'}until($depthText -in @('N','D'))
    Write-Host 'I/O selection: CONTROL = original | I1 = 64 KiB refill | I2 = resident seek | I1I2 = both'
    $ioChoices=@('CONTROL','I1','I2','I1I2')
    if($depthText -eq 'D'){$ioChoices+='D1';Write-Host 'D1 = optimized diagnostic catalog with original I/O'}
    do{$io=Read-Host 'I/O candidate (Enter = CONTROL)'}until($io -eq '' -or $io -in $ioChoices)
    if(-not $io){$io='CONTROL'}
    return Get-OptimizedMWHitchSelection -Mask ([int]$maskText) -TraceLevel $(if($depthText -eq 'D'){'Deep'}else{'Normal'}) -Candidate $io
}
