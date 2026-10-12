# Hermetic policy resolution. No game or user configuration is touched.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'OptimizedMW_MemoryProfile.ps1')
. (Join-Path $PSScriptRoot 'OptimizedMW_NumericMenu.ps1')
. (Join-Path $PSScriptRoot 'OptimizedMW_RunManifest.ps1')
$before=Get-Phase9EnvironmentSnapshot
try{
    foreach($name in @('Headroom','MatchedOverdrive','LegacyOverdrive')){
        foreach($ram in @(0,2,16,31)){
            $profile=Get-OptimizedMWMemoryProfile $name $ram
            $values=@{};foreach($entry in $profile.settings){$values[$entry[0]+'/'+$entry[1]]=$entry[2]}
            $v36=$values['V3/v3.6 performance profile'] -eq 'true'
            $disabled=$values['V3/v3.6 disable ram overdrive'] -eq 'true'
            # Mirrors the documented V3.6 precedence, which made the old profile ineffective.
            $effective=if($v36){if($disabled){'normal'}else{'overdrive'}}else{$values['Cells/ram cache mode']}
            if($effective -ne $(if($name -eq 'Headroom'){'normal'}else{'overdrive'})){throw 'V3.6 overrides the requested memory policy'}
            if($name -eq 'Headroom'){
                foreach($pair in @(@('cache expiry delay','120'),@('preload cell expiry delay','60'),@('preload cell cache min','16'),@('preload cell cache max','64'),@('prediction time','2'))){
                    if($values['Cells/'+$pair[0]] -cne $pair[1]){throw ('Missing headroom residence setting: '+$pair[0])}
                }
                if($profile.ram_mask -ne ($ram -bor 4)){throw 'Cold collision protection not enabled'}
            }elseif($profile.ram_mask -ne $ram){throw 'Control gained hidden RAM candidates'}
            Set-OptimizedMWMemoryProfileEnvironment $profile
            if($name -eq 'LegacyOverdrive'){
                if($env:OPENMW_RAM_BUDGET_V2 -ne '0' -or $profile.common_future_mb -ne 1024 -or $profile.experimental_total_mb -ne 0){throw 'Historical fallback changed'}
            }else{
                if($env:OPENMW_RAM_BUDGET_V2 -ne '1' -or $profile.common_future_mb -ne 256 -or $profile.experimental_total_mb -ne 256 -or $profile.experimental_scratch_mb -ne 128){throw 'Matched ownership allowances changed'}
            }
        }
    }
    $control=(Get-OptimizedMWNumericCandidates 1)[0]
    $lod=(Get-OptimizedMWNumericCandidates 12)[1]
    $mips=(Get-OptimizedMWNumericCandidates 2)[8]
    foreach($choice in @($control,$lod,$mips)){if($choice.memory_profile -ne 'Headroom'){throw 'Control/candidate memory cohorts differ'}}
    if((Get-OptimizedMWNumericCandidates 10)[8].memory_profile -ne 'LegacyOverdrive' -or
        (Get-OptimizedMWNumericCandidates 10)[9].memory_profile -ne 'MatchedOverdrive'){throw 'Fallback navigation lost'}
    Write-Output 'PASS: 12 memory policies; V3.6 precedence, explicit matched allowances, cold collision protection, numeric fallback and environment restoration'
}finally{if(-not (Restore-Phase9Environment $before)){throw 'Memory test failed to restore environment'}}
