$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'OptimizedMW_HitchMatrix.ps1')
. (Join-Path $PSScriptRoot 'OptimizedMW_AttackPresets.ps1')
. (Join-Path $PSScriptRoot 'OptimizedMW_QuickTest.ps1')
. (Join-Path $PSScriptRoot 'OptimizedMW_RunManifest.ps1')
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'OptimizedMW_Test.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors | Out-String)}
$fn=$ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Get-P9Mode'},$true)
. ([scriptblock]::Create($fn.Extent.Text))
$before=Get-Phase9EnvironmentSnapshot
try{
    $validMasks=@(0..511 | Where-Object {-not ($_ -band 128) -or ($_ -band 1)})
    foreach($mask in $validMasks){
        $hashes=@()
        foreach($depth in @('Normal','Deep')){
            $selection=Get-OptimizedMWHitchSelection 0 $depth CONTROL
            $mode=Get-P9Mode $selection.phase9_mode
            $combination=ConvertTo-OptimizedMWAttackSelection $selection $mode $mask
            Set-OptimizedMWHitchCombinationEnvironment $selection.features
            Set-OptimizedMWAttackEnvironment $combination
            $actual=@($env:OPENMW_HITCH_ANIMATION_LIFETIME,$env:OPENMW_HITCH_ANIMATION_PLAN,
                $env:OPENMW_HITCH_COMPOSITE_EARLY,$env:OPENMW_HITCH_WATER_PREPARE,
                $env:OPENMW_HITCH_WEATHER_PREPARE,$env:OPENMW_HITCH_LUA_PACKAGE_DESCRIPTORS,
                $env:OPENMW_P9_DELETE_ADMISSION,$env:OPENMW_HITCH_ANIMATION_WARM_LEASE,$env:OPENMW_HITCH_COMPOSITE_STEP)
            foreach($bit in 0..8){if($actual[$bit] -cne [string][int][bool]($mask -band (1 -shl $bit))){throw 'New mechanism environment inherited another selection'}}
            if($mode.Composite -ne [string][int][bool]($mask -band 260) -or
                $selection.features.animation_preload -ne [bool]($mask -band 1)){throw 'Mechanism prerequisite not explicit'}
            if($mode.HitchTrialRevision -ne 3 -or $env:OPENMW_P9_OBSERVER_LOCAL -ne '0'){throw 'Version/observer control changed'}
            $hashes+=Get-Phase9IdentityHash $selection.features
        }
        if($hashes[0] -cne $hashes[1]){throw 'New mechanism identity depends on diagnostic depth'}
    }
    $control=Get-OptimizedMWQuickPreset 5;$candidate=Get-OptimizedMWQuickPreset 6;$deep=Get-OptimizedMWQuickPreset 7
    if($control.mask -ne 4 -or $control.attack_mask -ne 0 -or $candidate.mask -ne 4 -or $candidate.attack_mask -ne 1 -or
        $control.system_trace -or $candidate.system_trace -or -not $deep.system_trace){throw 'First ANIM pair is not isolated Normal'}
    foreach($mask in @(-1,512,128,192,384)){
        $failed=$false;try{$null=Get-OptimizedMWAttackCombination $mask}catch{$failed=$true}
        if(-not $failed){throw 'Unsupported attack mask accepted'}
    }
    foreach($choice in @('8','9','10','11')){
        $preset=Get-OptimizedMWQuickPreset $choice
        $expected=if($choice -in @('8','9')){129}else{449}
        if($preset.attack_mask -ne $expected -or $preset.mask -ne 4 -or
            [bool]$preset.system_trace -ne ($choice -in @('9','11'))){throw 'Repair preset mapping changed'}
        if($choice -in @('9','11') -and -not $preset.gpu_query_defer){throw 'Repair diagnosis did not request deferred queries'}
    }
    Write-Host 'PASS 384 independent vectors/Normal-Deep identity, allrepairprerequisites, original1-7 and repair8-11 presets, environment restoration'
}finally{if(-not (Restore-Phase9Environment $before)){throw 'Attack test failed environment restoration'}}
