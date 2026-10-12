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
    # Retained function API; the production launcher uses category navigation.
    if(-not (Get-Command Read-OptimizedMWNumber -ErrorAction SilentlyContinue)){. (Join-Path $PSScriptRoot 'OptimizedMW_NumericMenu.ps1')}
    foreach($mask in 0..15){Write-Host (' {0,2} = {1}' -f ($mask+1),(Get-OptimizedMWHitchCombination $mask).name)}
    $choice=Read-OptimizedMWNumber 'Combination (1 = CONTROL, 16 = ALL)' @(1..16 | ForEach-Object {[string]$_})
    $depth=Read-OptimizedMWNumber 'Run type: 1 = Normal | 2 = Deep (Enter = 1)' @('1','2') '1'
    Write-Host 'File I/O: 1 = Original | 2 = Refill | 3 = Resident seek | 4 = Both'
    $choices=@('1','2','3','4');$names=@('CONTROL','I1','I2','I1I2')
    if($depth -eq '2'){Write-Host ' 5 = Diagnostic catalog collector, original I/O';$choices+='5';$names+='D1'}
    $io=Read-OptimizedMWNumber 'I/O candidate (Enter = 1)' $choices '1'
    return Get-OptimizedMWHitchSelection -Mask ([int]$choice-1) -TraceLevel $(if($depth -eq '2'){'Deep'}else{'Normal'}) -Candidate $names[[int]$io-1]
}
