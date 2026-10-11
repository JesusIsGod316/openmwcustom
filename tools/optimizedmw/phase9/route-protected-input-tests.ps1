$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'OptimizedMW_FrozenConfig.ps1')
. (Join-Path $PSScriptRoot 'OptimizedMW_RouteRecorder.ps1') -LibraryOnly

# Real-file integration tests. No game, user profile or existing route is touched.
$testRoot=[IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) ('OptimizedMW-ProtectedInputs-'+[Guid]::NewGuid().ToString('N'))))
[void][IO.Directory]::CreateDirectory($testRoot)
$results=[Collections.Generic.List[object]]::new()
function Assert-Check {
    param([bool]$Condition,[string]$Message)
    if(-not $Condition){throw $Message}
}
function Invoke-InputCase {
    param([string]$CaseName,[scriptblock]$Body)
    $root=Join-Path $testRoot $CaseName
    $original=Join-Path $root 'original';$private=Join-Path $root 'private';$privateData=Join-Path $private 'UserData'
    [void][IO.Directory]::CreateDirectory($original);[void][IO.Directory]::CreateDirectory($privateData)
    $names=@('openmw.cfg','settings.cfg','global_storage.bin','player_storage.bin','input_v3.xml','shaders.yaml','saves\Character\Start.omwsave')
    foreach($name in $names){
        $path=Join-Path $original $name
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
        [IO.File]::WriteAllText($path,'original '+$name)
    }
    foreach($name in @('openmw.cfg','settings.cfg','global_storage.bin','player_storage.bin')){[IO.File]::Copy((Join-Path $original $name),(Join-Path $private $name))}
    $baseline=[pscustomobject]@{files=@($names | ForEach-Object {[pscustomobject]@{relative_path=$_}});
        lock=[pscustomobject]@{source_configuration_directory=$original;source_user_data_directory=$original}}
    $runtime=[pscustomobject]@{directory=$private;user_data_directory=$privateData;
        config_path=(Join-Path $private 'openmw.cfg');settings_path=(Join-Path $private 'settings.cfg')}
    $before=Get-OptimizedMWRouteProtectedInputs $baseline
    $privateBefore=Get-OptimizedMWFrozenFile (Join-Path $private 'global_storage.bin')
    & $Body $baseline $runtime $before $original
    Assert-Check ((Get-OptimizedMWFrozenFile $privateBefore.path).sha256 -ceq $privateBefore.sha256) 'Review changed private Lua storage'
    $results.Add([pscustomobject]@{case=$CaseName;passed=$true})
}
try{
    Invoke-InputCase 'unchanged_default' {
        param($baseline,$runtime,$before,$original)
        $review=Get-OptimizedMWRouteProtectedInputReview -Before $before -After (Get-OptimizedMWRouteProtectedInputs $baseline) -Baseline $baseline -Runtime $runtime
        Assert-Check ($review.accepted -and $review.unchanged -and $review.changes.Count -eq 0) 'Unchanged strict inputs rejected'
    }
    Invoke-InputCase 'lua_strict_rejected' {
        param($baseline,$runtime,$before,$original)
        [IO.File]::WriteAllText((Join-Path $original 'global_storage.bin'),'external mod update')
        $review=Get-OptimizedMWRouteProtectedInputReview -Before $before -After (Get-OptimizedMWRouteProtectedInputs $baseline) -Baseline $baseline -Runtime $runtime
        Assert-Check (-not $review.accepted -and -not $review.unchanged -and $review.blocked_change_count -eq 1) 'Default policy ignored a mutation'
    }
    Invoke-InputCase 'both_lua_approved' {
        param($baseline,$runtime,$before,$original)
        foreach($name in @('global_storage.bin','player_storage.bin')){[IO.File]::WriteAllText((Join-Path $original $name),'external mod update '+$name)}
        $after=Get-OptimizedMWRouteProtectedInputs $baseline
        $review=Get-OptimizedMWRouteProtectedInputReview -Before $before -After $after -Baseline $baseline -Runtime $runtime -AllowExternalLuaStorageChanges
        Assert-Check ($review.accepted -and -not $review.unchanged -and $review.isolated_private_profile -and $review.changes.Count -eq 2) 'Approved external updates rejected or raw change hidden'
        Assert-Check ($review.changes[0].before.sha256 -ceq $before[2].sha256 -and $review.changes[0].after.sha256 -ceq $after[2].sha256) 'Exact before/after evidence lost'
        Assert-Check (@($review.changes | Where-Object {$_.reason -cne 'approved_external_lua_storage_update'}).Count -eq 0) 'Unexpected approval reason'
    }
    foreach($name in @('openmw.cfg','settings.cfg','input_v3.xml','shaders.yaml','saves\Character\Start.omwsave')){
        $body={
            param($baseline,$runtime,$before,$original)
            [IO.File]::WriteAllText((Join-Path $original $name),'changed protected input')
            [IO.File]::WriteAllText((Join-Path $original 'player_storage.bin'),'approved external change')
            $review=Get-OptimizedMWRouteProtectedInputReview -Before $before -After (Get-OptimizedMWRouteProtectedInputs $baseline) -Baseline $baseline -Runtime $runtime -AllowExternalLuaStorageChanges
            Assert-Check (-not $review.accepted -and $review.blocked_change_count -eq 1 -and $review.changes.Count -eq 2) ('Lua approval swallowed '+$name)
        }.GetNewClosure()
        Invoke-InputCase ('protected_'+[IO.Path]::GetFileNameWithoutExtension($name)) $body
    }
    Invoke-InputCase 'deleted_lua_rejected' {
        param($baseline,$runtime,$before,$original)
        [IO.File]::Delete((Join-Path $original 'global_storage.bin'))
        $review=Get-OptimizedMWRouteProtectedInputReview -Before $before -After (Get-OptimizedMWRouteProtectedInputs $baseline) -Baseline $baseline -Runtime $runtime -AllowExternalLuaStorageChanges
        Assert-Check (-not $review.accepted -and $review.blocked_change_count -eq 1) 'Lua deletion treated as an approved update'
    }
    Invoke-InputCase 'private_lookalike_rejected' {
        param($baseline,$runtime,$before,$original)
        $private=Join-Path $runtime.directory 'global_storage.bin'
        $old=Get-OptimizedMWFrozenFile $private;$old | Add-Member -NotePropertyName exists -NotePropertyValue $true
        # Retain the file itself; test rejection of a private identity in the review.
        $new=[pscustomobject]@{path=$private;exists=$true;sha256=('1'*64);bytes=$old.bytes}
        $review=Get-OptimizedMWRouteProtectedInputReview -Before @($old) -After @($new) -Baseline $baseline -Runtime $runtime -AllowExternalLuaStorageChanges
        Assert-Check (-not $review.accepted) 'Private same-name Lua file was allowlisted'
    }
    Invoke-InputCase 'identity_structure_rejected' {
        param($baseline,$runtime,$before,$original)
        $after=Get-OptimizedMWRouteProtectedInputs $baseline
        $review=Get-OptimizedMWRouteProtectedInputReview -Before $before -After @($after | Select-Object -Skip 1) -Baseline $baseline -Runtime $runtime -AllowExternalLuaStorageChanges
        Assert-Check (-not $review.accepted) 'Removed identity accepted'
        $reordered=@($after[1],$after[0])+$after[2..($after.Count-1)]
        $review=Get-OptimizedMWRouteProtectedInputReview -Before $before -After $reordered -Baseline $baseline -Runtime $runtime -AllowExternalLuaStorageChanges
        Assert-Check (-not $review.accepted) 'Changed identity order accepted'
    }
    Invoke-InputCase 'profile_alias_rejected' {
        param($baseline,$runtime,$before,$original)
        $runtime.directory=$original;$runtime.user_data_directory=Join-Path $original 'UserData'
        $runtime.config_path=Join-Path $original 'openmw.cfg';$runtime.settings_path=Join-Path $original 'settings.cfg'
        $caught=$false
        try{$null=Get-OptimizedMWRouteProtectedInputReview -Before $before -After $before -Baseline $baseline -Runtime $runtime -AllowExternalLuaStorageChanges}catch{$caught=$_.Exception.Message -match 'disjoint'}
        Assert-Check $caught 'Shared original/private profile accepted'
    }
    Invoke-InputCase 'external_userdata_rejected' {
        param($baseline,$runtime,$before,$original)
        $runtime.user_data_directory=Join-Path $original 'UserData'
        $caught=$false
        try{$null=Get-OptimizedMWRouteProtectedInputReview -Before $before -After $before -Baseline $baseline -Runtime $runtime -AllowExternalLuaStorageChanges}catch{$caught=$_.Exception.Message -match 'private config and user-data'}
        Assert-Check $caught 'Non-private user-data accepted'
    }
    [pscustomobject]@{schema=1;passed=$results.Count;failed=0;game_instances_started=0;cases=$results.ToArray()} | ConvertTo-Json -Depth 5
}finally{
    $resolved=[IO.Path]::GetFullPath($testRoot)
    $expected=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/')+'\'
    if(-not $resolved.StartsWith($expected,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolved) -cnotmatch '^OptimizedMW-ProtectedInputs-[a-f0-9]{32}$'){throw 'Refusing unsafe test cleanup target'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
