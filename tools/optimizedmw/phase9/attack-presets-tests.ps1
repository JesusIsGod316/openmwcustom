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
            $env:OPENMW_HITCH_COMPOSITE_PROGRESS='1';$env:OPENMW_HITCH_COMPOSITE_CPU_MIPS='1'
            Set-OptimizedMWAttackEnvironment $combination
            if($env:OPENMW_HITCH_COMPOSITE_PROGRESS -ne '0' -or $env:OPENMW_HITCH_COMPOSITE_CPU_MIPS -ne '0'){throw 'Old attack selection inherited a renderer repair'}
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
    foreach($entry in @(@('CONTROL',1,0,0),@('CLEANUP',65,0,0),@('STEP',257,0,0),
        @('EARLY-STEP',261,1,0),@('TERRAIN-MIPS',261,1,1),@('COMBINED',325,1,0),@('COMBINED-MIPS',325,1,1))){
        $hashes=@()
        foreach($depth in @('Normal','Deep')){
            $preset=Get-OptimizedMWRendererPreset $entry[0] $depth
            if($preset.attack_mask -ne $entry[1] -or $preset.composite_progress -ne [bool]$entry[2] -or
                $preset.composite_cpu_mips -ne [bool]$entry[3] -or $preset.system_trace -or
                $preset.gpu_query_defer -ne ($depth -eq 'Deep')){throw 'Focused renderer preset lost its explicit selection'}
            $selection=Get-OptimizedMWHitchSelection $preset.mask $depth CONTROL
            $mode=Get-P9Mode $selection.phase9_mode
            $combination=ConvertTo-OptimizedMWAttackSelection $selection $mode $preset.attack_mask
            $identity=Set-OptimizedMWRendererIdentity $selection $mode $preset.focused_preset -CompositeProgress:$preset.composite_progress -CompositeCpuMips:$preset.composite_cpu_mips
            Set-OptimizedMWAttackEnvironment $combination -CompositeProgress:$preset.composite_progress -CompositeCpuMips:$preset.composite_cpu_mips
            if($env:OPENMW_HITCH_COMPOSITE_PROGRESS -cne [string]$entry[2] -or $env:OPENMW_HITCH_COMPOSITE_CPU_MIPS -cne [string]$entry[3] -or
                $combination.animation_warm_lease -or $combination.weather_prepare -or $mode.FocusedRendererTrialRevision -ne 1){throw 'Focused renderer environment/prerequisites changed'}
            $hashes+=Get-Phase9IdentityHash $selection.features
        }
        if($hashes[0] -cne $hashes[1]){throw 'Renderer gameplay identity changed with diagnostic depth'}
    }
    $failed=$false;try{$null=Get-OptimizedMWRendererPreset TERRAIN-MIPS Normal -SystemTrace}catch{$failed=$true}
    if(-not $failed){throw 'Normal renderer preset accepted Windows recording'}
    if(-not (Get-OptimizedMWRendererPreset TERRAIN-MIPS Deep -SystemTrace).system_trace){throw 'Explicit Deep system recording lost'}
    foreach($progress in @($false,$true)){
        foreach($mips in @($false,$true)){
            $selection=Get-OptimizedMWHitchSelection 4 Normal CONTROL;$mode=Get-P9Mode $selection.phase9_mode
            $combination=ConvertTo-OptimizedMWAttackSelection $selection $mode 1
            $null=Set-OptimizedMWRendererIdentity $selection $mode CUSTOM -CompositeProgress:$progress -CompositeCpuMips:$mips
            Set-OptimizedMWAttackEnvironment $combination -CompositeProgress:$progress -CompositeCpuMips:$mips
            if($selection.features.renderer.composite_progress -ne $progress -or $selection.features.renderer.composite_cpu_mips -ne $mips -or
                $env:OPENMW_HITCH_COMPOSITE_PROGRESS -cne [string][int]$progress -or $env:OPENMW_HITCH_COMPOSITE_CPU_MIPS -cne [string][int]$mips -or
                $selection.features.attack.mask -ne 1){throw 'Independent renderer switches changed each other or overloaded historic mask'}
        }
    }
    Write-Host 'PASS 384 independent vectors/Normal-Deep identity, original1-11 semantics, seven focused renderer selections, inherited flag reset and environment restoration'
}finally{if(-not (Restore-Phase9Environment $before)){throw 'Attack test failed environment restoration'}}
