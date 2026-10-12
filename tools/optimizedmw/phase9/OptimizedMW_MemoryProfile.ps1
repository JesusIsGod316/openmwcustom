# Startup policy, not a whole-process RAM ceiling. Settings are restored by the launcher.
function Get-OptimizedMWMemoryProfile {
    param([ValidateSet('Headroom','MatchedOverdrive','LegacyOverdrive','Configured')][string]$Name='Headroom',
        [ValidateRange(0,31)][int]$RamMask=0)
    if($Name -eq 'Configured'){
        return [pscustomobject][ordered]@{revision=2;name=$Name;settings=@();ram_mask=$RamMask;
            ownership_policy_enabled=$true;common_future_mb=$null;experimental_total_mb=$null;experimental_scratch_mb=$null;
            retention_scope='Individual settings from the hashed custom file; effective values require startup receipt';
            comparison_scope='Custom configuration cohort; compare only identical configuration hashes'}
    }
    $legacy=$Name -eq 'LegacyOverdrive'
    $headroom=$Name -eq 'Headroom'
    $settings=@(
        @('V3','v3.6 performance profile','true'),
        @('V3','v3.6 disable ram overdrive',$(if($headroom){'true'}else{'false'})),
        @('Cells','ram cache mode',$(if($headroom){'normal'}else{'overdrive'})),
        @('Cells','opimizedmw transient budget mb',$(if($legacy){'1024'}else{'256'})))
    if(-not $legacy){$settings+=@(@('Optimizations','experimental cache budget mb','256'),
        @('Optimizations','experimental scratch budget mb','128'),@('Optimizations','experimental concurrent builds','1'))}
    if($headroom){
        $settings+=@(@('Cells','cache expiry delay','120'),
            @('Cells','preload cell expiry delay','60'),
            @('Cells','preload cell cache min','16'),
            @('Cells','preload cell cache max','64'),
            @('Cells','prediction time','2'),@('Cells','preload instances','true'))
    }
    return [pscustomobject][ordered]@{revision=2;name=$Name;settings=$settings;
        ram_mask=$(if($headroom){$RamMask -bor 4}else{$RamMask});
        ownership_policy_enabled=(-not $legacy);common_future_mb=$(if($legacy){1024}else{256});
        experimental_total_mb=$(if($legacy){0}else{256});experimental_scratch_mb=$(if($legacy){0}else{128});
        retention_scope='Residence times and cold counts; protected/live resources and other process allocations are outside these controls';
        comparison_scope=$(if($legacy){'Historical policy: asset modes clamp the common allowance to 256 MiB'}else{'Same common and experimental policy for control and candidate; compare within this memory profile'})}
}
function Set-OptimizedMWMemoryProfileEnvironment {
    param([Parameter(Mandatory=$true)][object]$Profile)
    $env:OPENMW_RAM_BUDGET_V2=[string][int][bool]$Profile.ownership_policy_enabled
}
