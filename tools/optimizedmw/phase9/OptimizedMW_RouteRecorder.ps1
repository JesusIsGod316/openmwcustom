param(
    [string]$GameDir=$PSScriptRoot,
    [string]$FrozenConfigDirectory='',
    [ValidateSet('','existing','balmora')][string]$Route='',
    [string]$RecorderPackageDirectory='',
    [switch]$PrepareOnly,
    [switch]$Smoke,
    [switch]$ValidationWindow,
    [switch]$AnimationMovement,
    [ValidateRange(5,30)][int]$SmokeSeconds=20,
    [switch]$LibraryOnly
)
$ErrorActionPreference='Stop'

function Write-OptimizedMWRouteJson {
    param([string]$Path,[object]$Value)
    [IO.File]::WriteAllText($Path,($Value | ConvertTo-Json -Depth 24),[Text.UTF8Encoding]::new($false))
}

function Get-OptimizedMWRouteProtectedInputs {
    param([object]$Baseline)
    $result=[Collections.Generic.List[object]]::new()
    foreach($file in $Baseline.files){
        $relative=[string]$file.relative_path
        if($relative -notin @('openmw.cfg','settings.cfg','global_storage.bin','player_storage.bin','input_v3.xml','shaders.yaml') -and $relative -notmatch '^saves[\\/]'){continue}
        $root=[string]$Baseline.lock.source_configuration_directory
        if($relative -match '^saves[\\/]'){$root=[string]$Baseline.lock.source_user_data_directory}
        $path=Assert-OptimizedMWPlainPath (Join-Path $root $relative)
        if([IO.File]::Exists($path)){$identity=Get-OptimizedMWFrozenFile $path;$result.Add([pscustomobject]@{path=$path;exists=$true;sha256=$identity.sha256;bytes=$identity.bytes})}
        else{$result.Add([pscustomobject]@{path=$path;exists=$false;sha256=$null;bytes=0})}
    }
    return $result.ToArray()
}

function Compare-OptimizedMWRouteProtectedInputs {
    param([object[]]$Before,[object[]]$After)
    $unchanged=$Before.Count -eq $After.Count
    for($i=0;$i -lt $Before.Count -and $i -lt $After.Count;++$i){
        if($Before[$i].path -ine $After[$i].path -or $Before[$i].exists -ne $After[$i].exists -or $Before[$i].sha256 -cne $After[$i].sha256 -or $Before[$i].bytes -ne $After[$i].bytes){$unchanged=$false}
    }
    return $unchanged
}

function Set-OptimizedMWRouteIniValue {
    param([string]$Path,[string]$Section,[string]$Key,[string]$Value)
    $lines=[Collections.Generic.List[string]]::new();$current='';$inserted=$false
    foreach($line in [IO.File]::ReadAllLines($Path)){
        if($line -match '^\s*\[([^\]]+)\]\s*$'){
            if($current -ieq $Section -and -not $inserted){$lines.Add($Key+' = '+$Value);$inserted=$true}
            $current=$matches[1].Trim()
        }
        if($current -ieq $Section -and $line -match ('^\s*'+[regex]::Escape($Key)+'\s*=')){continue}
        $lines.Add($line)
    }
    if(-not $inserted){if($current -ine $Section){$lines.Add('');$lines.Add('['+$Section+']')};$lines.Add($Key+' = '+$Value)}
    [IO.File]::WriteAllLines($Path,$lines,[Text.UTF8Encoding]::new($false))
}

function Protect-OptimizedMWRouteNativeBindings {
    param([string]$Path)
    $settings=[Xml.XmlReaderSettings]::new();$settings.DtdProcessing=[Xml.DtdProcessing]::Prohibit;$settings.XmlResolver=$null
    $reader=[Xml.XmlReader]::Create($Path,$settings)
    $document=[Xml.XmlDocument]::new();$document.PreserveWhitespace=$true;$document.XmlResolver=$null
    try{$document.Load($reader)}finally{$reader.Dispose()}
    if($document.DocumentElement.Name -cne 'Controller'){throw 'Unsupported private native input schema'}
    $parked=[Collections.Generic.List[object]]::new()
    $parking=@{'63'='112';'66'='113';'67'='114';'68'='115'}
    foreach($node in @($document.SelectNodes('//KeyBinder'))){
        if([string]$node.GetAttribute('key') -in @('112','113','114','115')){throw 'Private input already uses a reserved F21-F24 parking key'}
    }
    foreach($node in @($document.SelectNodes('//KeyBinder'))){
        $old=[string]$node.GetAttribute('key')
        if($parking.ContainsKey($old)){
            $before=$node.OuterXml
            $node.SetAttribute('key',$parking[$old])
            $parked.Add([pscustomobject]@{original_scancode=$old;parked_scancode=$parking[$old];control=$node.ParentNode.GetAttribute('name');original_binding=$before;private_binding=$node.OuterXml})
        }
    }
    $writerSettings=[Xml.XmlWriterSettings]::new();$writerSettings.Encoding=[Text.UTF8Encoding]::new($false);$writerSettings.Indent=$false
    $writer=[Xml.XmlWriter]::Create($Path,$writerSettings)
    try{$document.Save($writer)}finally{$writer.Dispose()}
    return $parked.ToArray()
}

function New-OptimizedMWRouteSession {
    param([string]$GameDirectory,[string]$FrozenDirectory,[string]$PackageDirectory,[string]$RouteName,[switch]$SmokeRun,[int]$SmokeDuration=20,[switch]$SmallValidationWindow,[ValidateSet('Routes','Playback')][string]$OutputCategory='Routes',[switch]$PlaybackCompatible,[switch]$NativeMovement)
    if($RouteName -notin @('existing','balmora')){throw 'Choose existing or balmora'}
    if($SmokeDuration -lt 5 -or $SmokeDuration -gt 30){throw 'Smoke duration must be 5-30 seconds'}
    $game=Assert-OptimizedMWPlainPath $GameDirectory
    $package=Assert-OptimizedMWPlainPath $PackageDirectory
    $baseline=Read-OptimizedMWFrozenLock $FrozenDirectory
    $boundStart=$null
    if($baseline.lock.PSObject.Properties['route_start']){
        $boundStart=$baseline.lock.route_start
        if($boundStart.schema -ne 1 -or [string]$boundStart.route -cne $RouteName -or [string]$boundStart.mode -cne 'load_saved_location' -or
            [string]$boundStart.save_relative_path -cne $baseline.save.relative_path -or [string]$boundStart.save_sha256 -cne $baseline.save.sha256 -or
            [string]::IsNullOrWhiteSpace([string]$boundStart.saved_cell)){
            throw 'Frozen route starting-save binding does not match the selected route/save'
        }
    }
    if($RouteName -eq 'balmora' -and -not $SmokeRun -and -not $boundStart){throw 'Balmora requires its dedicated frozen starting-save binding'}
    $encounterPolicyPath=Join-Path $package 'encounter-policy.json'
    $encounterPolicyIdentity=Get-OptimizedMWFrozenFile $encounterPolicyPath
    if($encounterPolicyIdentity.bytes -gt 1048576){throw 'Route encounter policy exceeds 1 MiB'}
    $encounterPolicy=[IO.File]::ReadAllText($encounterPolicyPath) | ConvertFrom-Json
    if($encounterPolicy.schema -ne 1){throw 'Unsupported route encounter policy schema'}
    $configIdentity=Get-OptimizedMWFrozenFile (Join-Path $baseline.directory 'openmw.cfg')
    if([string]$encounterPolicy.frozen_config_sha256 -ine $configIdentity.sha256){throw 'Route encounter policy does not match the frozen mod configuration'}
    $contentFiles=@($encounterPolicy.content_files)
    if($contentFiles.Count -lt 1 -or $contentFiles.Count -gt 256){throw 'Route encounter policy requires 1-256 bound content files'}
    $seenContent=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach($file in $contentFiles){
        if(-not [IO.Path]::IsPathRooted([string]$file.path) -or -not $seenContent.Add([string]$file.path)){throw 'Invalid or duplicate route encounter content path'}
        $identity=Get-OptimizedMWFrozenFile $file.path
        if([string]$file.sha256 -notmatch '^[a-fA-F0-9]{64}$' -or $identity.sha256 -ine [string]$file.sha256 -or $null -eq $file.bytes -or $identity.bytes -ne $file.bytes){throw ('Route encounter content changed since audit: '+$file.path)}
    }
    # User development mods retain observed identities without an exact-version gate.
    $mutableFiles=@()
    if($encounterPolicy.PSObject.Properties['mutable_development_content_files']){$mutableFiles=@($encounterPolicy.mutable_development_content_files)}
    if($mutableFiles.Count -gt 32){throw 'Too many mutable development content files'}
    $mutableIdentities=[Collections.Generic.List[object]]::new()
    foreach($file in $mutableFiles){
        if(-not [IO.Path]::IsPathRooted([string]$file.path) -or -not $seenContent.Add([string]$file.path) -or
            [string]::IsNullOrWhiteSpace([string]$file.content) -or [IO.Path]::GetFileName([string]$file.path) -ine [string]$file.content){
            throw 'Invalid, duplicate or bound mutable development content path'
        }
        $identity=Get-OptimizedMWFrozenFile $file.path
        $mutableIdentities.Add([pscustomobject]@{content=[string]$file.content;path=$identity.path;sha256=$identity.sha256;bytes=$identity.bytes;binding='observed_at_preparation_not_required_version'})
    }
    $hostileRecords=@($encounterPolicy.hostile_record_ids)
    if($hostileRecords.Count -lt 1 -or $hostileRecords.Count -gt 4096){throw 'Route encounter policy must have 1-4096 explicit records'}
    $seenRecords=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach($record in $hostileRecords){
        if($record -isnot [string] -or $record -cnotmatch '^[a-z0-9_][a-z0-9_ .:-]{0,511}$' -or -not $seenRecords.Add($record)){throw 'Invalid or duplicate route encounter record'}
    }
    $hostileRecordsLua=(@($hostileRecords | ForEach-Object {"'"+$_+"'"}) -join ',')
    if((Get-OptimizedMWFrozenFile $encounterPolicyPath).sha256 -cne $encounterPolicyIdentity.sha256){throw 'Route encounter policy changed during validation'}
    $protected=Get-OptimizedMWRouteProtectedInputs $baseline
    $exe=Get-OptimizedMWFrozenFile (Join-Path $game 'openmw.exe')
    $packageFiles=@(Get-ChildItem -LiteralPath $package -File -Recurse -Force)
    if($packageFiles.Count -lt 2 -or $packageFiles.Count -gt 64){throw 'Recorder package must contain 2-64 files'}
    $packageBytes=0L;$packageIdentities=[Collections.Generic.List[object]]::new()
    foreach($file in $packageFiles){
        $identity=Get-OptimizedMWFrozenFile $file.FullName
        $packageBytes+=$identity.bytes
        if($packageBytes -gt 1048576){throw 'Recorder package exceeds 1 MiB'}
        $relative=$identity.path.Substring($package.TrimEnd('\','/').Length+1)
        if($relative -match '(^|[\\/])\.\.?([\\/]|$)|:|[\r\n]|(^|[\\/])[^\\/]*[. ]([\\/]|$)'){throw 'Unsafe recorder package path'}
        $packageIdentities.Add([pscustomobject]@{relative_path=$relative;path=$identity.path;sha256=$identity.sha256;bytes=$identity.bytes})
    }
    $content='optimizedmw_route_recorder.omwscripts'
    if(-not [IO.File]::Exists((Join-Path $package $content))){throw 'Recorder omwscripts content missing'}
    $runtime=New-OptimizedMWFrozenRuntime -Directory $FrozenDirectory -GameDir $game
    $validatedFrozen=Add-OptimizedMWFrozenManifest -Manifest ([pscustomobject]@{scope='verified_frozen_inputs_before_recorder_derivation'}) -Runtime $runtime
    $id=[Guid]::NewGuid().ToString('N')
    $routes=Assert-OptimizedMWPlainPath (Join-Path $game ('Benchmarks\'+$OutputCategory))
    $output=Join-Path $routes ($RouteName+'_'+$id)
    [void][IO.Directory]::CreateDirectory($output)
    $data=Join-Path $runtime.user_data_directory 'data'
    foreach($file in $packageIdentities){
        $target=Join-Path $data $file.relative_path
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
        [IO.File]::Copy($file.path,$target)
        if((Get-OptimizedMWFrozenFile $target).sha256 -cne $file.sha256){throw 'Recorder package copy mismatch'}
    }
    $optionsPath=Join-Path $data 'scripts\optimizedmw_route_recorder\options.lua'
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($optionsPath))
    $boolean=$(if($SmokeRun){'true'}else{'false'})
    $compatible=$(if($PlaybackCompatible){'true'}else{'false'})
    $movementCohort=$null;$movementOptions='';$cameraCohort=$null;$cameraOptions='';$maxSeconds=600;$maxWallSeconds=600
    if($PlaybackCompatible -or $NativeMovement){
        $movementCohort=[pscustomobject]@{model=$(if($NativeMovement){'native_input_displacement_with_visual_animations'}else{'animation_displacement'});private_override=[bool]$NativeMovement}
        $override=$(if($NativeMovement){'true'}else{'false'})
        Set-OptimizedMWRouteIniValue -Path $runtime.settings_path -Section 'Game' -Key 'player movement ignores animation' -Value $override
        $movementOptions=", movement_cohort={model='"+$movementCohort.model+"',private_override="+$override+'}'
    }
    if($PlaybackCompatible){
        $maxSeconds=180;$maxWallSeconds=300
        $cameraCohort=[pscustomobject]@{model='recorded_additive_first_person_v1'}
        $cameraOptions=", camera_layer_cohort={model='recorded_additive_first_person_v1'}"
    }
    $expectedCell=$(if($RouteName -eq 'balmora'){'Balmora'}else{'Seyda Neen'})
    # Admit the audited encounter-list closure, not only one observed spawn.
    # Lua still confirms the queued disable and rejects any observed combat.
    $options="return { enabled=true, schema=1, session='$id', route_name='$RouteName', checkpoint_save_required=true, playback_compatible=$compatible$movementOptions$cameraOptions, no_enemy_policy={enabled=true, discover_high_fight_creatures=true, creature_fight_threshold=90, hostile_reference_ids={}, hostile_record_ids={$hostileRecordsLua}}, smoke_autostart=$boolean, smoke_duration_seconds=$SmokeDuration, smoke_quit=$boolean, smoke_expected_cell='$expectedCell', quit_on_finish=true, require_ctrl_shift=false, key_start='F6', key_checkpoint='F9', key_finish='F10', key_abort='F11', max_seconds=$maxSeconds, max_wall_seconds=$maxWallSeconds, max_records=60000, max_bytes=16777216, sample_hz=20 }"
    [IO.File]::WriteAllText($optionsPath,$options,[Text.UTF8Encoding]::new($false))
    [IO.File]::AppendAllText($runtime.config_path,"`ncontent=$content`n",[Text.UTF8Encoding]::new($false))
    # Recording is route setup, not a performance capture. Preserve gameplay
    # foundations but disable observers so their logging cannot skew recording.
    foreach($setting in @(@('Optimizations','render handoff attribution','false'),@('Optimizations','async gpu profiler','false'),@('V3','v3.6 async gpu profiler','false'),@('Lua','lua profiler','false'))){
        Set-OptimizedMWRouteIniValue -Path $runtime.settings_path -Section $setting[0] -Key $setting[1] -Value $setting[2]
    }
    $bindingPath=Join-Path $runtime.directory 'input_v3.xml'
    # Native loading restores defaults for an action with no key/mouse/wheel
    # binder. Retain explicit unused F21-F24 bindings instead of deleting them.
    $parkedBindings=@(Protect-OptimizedMWRouteNativeBindings $bindingPath)
    if($SmallValidationWindow){
        foreach($setting in @(@('Video','fullscreen','false'),@('Video','resolution x','1280'),@('Video','resolution y','720'))){Set-OptimizedMWRouteIniValue -Path $runtime.settings_path -Section $setting[0] -Key $setting[1] -Value $setting[2]}
    }
    $arguments=@(Get-OptimizedMWFrozenArguments $runtime)+@('--load-savegame',('"'+$runtime.save_path+'"'),'--skip-menu')
    # A console setup script executes only once the loaded game is running.
    # COC resolves the modded city from its cell name; no coordinates are guessed.
    $setup=$null
    if(($RouteName -eq 'balmora' -and -not $boundStart) -or $SmokeRun){
        $setupCell=$(if($RouteName -eq 'balmora'){'Balmora'}else{'Seyda Neen'})
        $setup=Join-Path $runtime.directory 'City-Setup.txt'
        [IO.File]::WriteAllText($setup,('coc "'+$setupCell+'"'+"`n"),[Text.UTF8Encoding]::new($false))
        $arguments+=@('--script-run',('"'+$setup+'"'))
    }
    $manifest=[pscustomobject][ordered]@{schema=1;session=$id;route=$RouteName;mode=$(if($SmokeRun){'bounded_startup_smoke_not_route_acceptance'}else{'guided_recording_no_automatic_replay'});
        created_utc=[DateTime]::UtcNow.ToString('o');executable=$exe;recorder_package=$packageIdentities.ToArray();options=(Get-OptimizedMWFrozenFile $optionsPath);
        host_tooling=@((Get-OptimizedMWFrozenFile (Join-Path $PSScriptRoot 'OptimizedMW_RouteRecorder.ps1')),(Get-OptimizedMWFrozenFile (Join-Path $PSScriptRoot 'OptimizedMW_FrozenConfig.ps1')));
        protected_before=$protected;arguments=$arguments;runtime_directory=$runtime.directory;output_directory=$output;
        stdout_path=(Join-Path $output 'stdout.log');stderr_path=(Join-Path $output 'stderr.log');
        setup_script=$(if($setup){Get-OptimizedMWFrozenFile $setup}else{$null});
        scope='Route recorder only. No optimization matrix, Windows trace, automated replay, or benchmark performance claim.';
        actor_policy='Host does not remove actors. Lua policy and unexpected combat qualification are recorded separately.'}
    $manifest | Add-Member -NotePropertyName route_start -NotePropertyValue $boundStart
    $manifest | Add-Member -NotePropertyName starting_location_mode -NotePropertyValue $(if($setup){'diagnostic_coc_setup_not_saved_location'}else{'load_saved_location'})
    $manifest | Add-Member -NotePropertyName encounter_policy -NotePropertyValue ([pscustomobject]@{identity=$encounterPolicyIdentity;frozen_config_sha256=$encounterPolicy.frozen_config_sha256;explicit_record_count=$hostileRecords.Count;verified_content_files=$contentFiles.Count;mutable_development_content=$mutableIdentities.ToArray();scope=$encounterPolicy.scope})
    $manifest | Add-Member -NotePropertyName native_recorder_bindings_parked -NotePropertyValue $parkedBindings
    $manifest | Add-Member -NotePropertyName validation_window -NotePropertyValue ([bool]$SmallValidationWindow)
    $manifest | Add-Member -NotePropertyName playback_compatible -NotePropertyValue ([bool]$PlaybackCompatible)
    $manifest | Add-Member -NotePropertyName movement_cohort -NotePropertyValue $movementCohort
    $manifest | Add-Member -NotePropertyName camera_layer_cohort -NotePropertyValue $cameraCohort
    $manifest | Add-Member -NotePropertyName maximum_acquisition_simulation_seconds -NotePropertyValue $(if($PlaybackCompatible){$maxSeconds}else{$null})
    $manifest | Add-Member -NotePropertyName maximum_acquisition_wall_seconds -NotePropertyValue $maxWallSeconds
    $manifest | Add-Member -NotePropertyName frozen_configuration -NotePropertyValue $validatedFrozen.frozen_configuration
    $manifest | Add-Member -NotePropertyName derived_private_inputs -NotePropertyValue @((Get-OptimizedMWFrozenFile $runtime.config_path),(Get-OptimizedMWFrozenFile $runtime.settings_path),(Get-OptimizedMWFrozenFile $bindingPath))
    Write-OptimizedMWRouteJson (Join-Path $output 'SESSION.json') $manifest
    return [pscustomobject]@{runtime=$runtime;baseline=$baseline;manifest=$manifest;directory=$output;id=$id;arguments=$arguments;executable=$exe.path}
}

function Test-OptimizedMWRouteNumber {
    param($Value,[double]$Minimum=-100000000,[double]$Maximum=100000000)
    if($null -eq $Value -or $Value -is [string] -or $Value -is [bool]){return $false}
    try{$number=[double]$Value}catch{return $false}
    return -not [double]::IsNaN($number) -and -not [double]::IsInfinity($number) -and $number -ge $Minimum -and $number -le $Maximum
}

function Test-OptimizedMWRouteCameraCohort {
    param($Cohort)
    if($null -eq $Cohort){return $true}
    return $Cohort -is [pscustomobject] -and @($Cohort.PSObject.Properties).Count -eq 1 -and
        $Cohort.model -is [string] -and $Cohort.model -ceq 'recorded_additive_first_person_v1'
}

function Test-OptimizedMWRouteCameraLayers {
    param($Camera,[switch]$Readback)
    if($null -eq $Camera){return $false}
    $angleLimit=[Math]::PI+$(if($Readback){0.000001}else{0})
    foreach($angle in @('extra_yaw','extra_pitch','roll','extra_roll')){
        if(-not (Test-OptimizedMWRouteNumber $Camera.$angle (-$angleLimit) $angleLimit)){return $false}
    }
    $offset=$Camera.first_person_offset
    if($offset -isnot [pscustomobject] -or @($offset.PSObject.Properties).Count -ne 3){return $false}
    foreach($axis in @('x','y','z')){if(-not (Test-OptimizedMWRouteNumber $offset.$axis -1024 1024)){return $false}}
    return $true
}

function Test-OptimizedMWRouteFiniteTree {
    param($Value,[int]$Depth=0)
    if($Depth -gt 16){return $false}
    if($null -eq $Value -or $Value -is [bool] -or $Value -is [string]){return $true}
    if($Value -is [ValueType]){return (Test-OptimizedMWRouteNumber $Value -1.0e16 1.0e16)}
    if($Value -is [Array]){
        if($Value.Count -gt 4096){return $false}
        foreach($item in $Value){if(-not (Test-OptimizedMWRouteFiniteTree $item ($Depth+1))){return $false}}
        return $true
    }
    foreach($property in $Value.PSObject.Properties){if(-not (Test-OptimizedMWRouteFiniteTree $property.Value ($Depth+1))){return $false}}
    return $true
}

function Assert-OptimizedMWRouteSessionInputs {
    param([object]$Session)
    $baseline=Read-OptimizedMWFrozenLock $Session.runtime.baseline_directory
    if($baseline.lock_identity.sha256 -cne $Session.runtime.baseline_lock.sha256){throw 'Frozen baseline changed before recorder launch'}
    foreach($identity in @($Session.manifest.executable,$Session.manifest.options,$Session.runtime.copied_save_identity)+@($Session.manifest.derived_private_inputs)+@($Session.manifest.host_tooling)){
        $actual=Get-OptimizedMWFrozenFile $identity.path
        if($actual.sha256 -cne $identity.sha256 -or $actual.bytes -ne $identity.bytes){throw 'Recorder prepared input changed before launch'}
    }
    foreach($identity in @($Session.runtime.copied_input_identities)){
        if([IO.Path]::GetFileName($identity.path) -in @('settings.cfg','input_v3.xml')){continue}
        if((Get-OptimizedMWFrozenFile $identity.path).sha256 -cne $identity.sha256){throw 'Private persistent input changed before launch'}
    }
    foreach($identity in $Session.manifest.recorder_package){
        if($identity.relative_path.Replace('\','/') -ceq 'scripts/optimizedmw_route_recorder/options.lua'){continue}
        $actual=Get-OptimizedMWFrozenFile (Join-Path (Join-Path $Session.runtime.user_data_directory 'data') $identity.relative_path)
        if($actual.sha256 -cne $identity.sha256){throw 'Private recorder package changed before launch'}
    }
    $null=Test-OptimizedMWFrozenSaintAssets -Baseline $baseline -OrderedData $Session.runtime.ordered_data_paths
}

function Export-OptimizedMWRouteLog {
    param([string]$LogPath,[string]$OutputPath,[string]$SessionId,[Nullable[int]]$ExitCode)
    if($SessionId -notmatch '^[a-f0-9]{32}$'){throw 'Invalid route session identity'}
    $errors=[Collections.Generic.List[string]]::new();$events=[Collections.Generic.Dictionary[string,int]]::new()
    $seq=0L;$lastTime=-1.0;$lastSimulation=-1.0;$lastFrame=-1L;$started=$false;$finished=$false;$startTime=0.0;$finish=$null;$bytes=0L;$cellKnown=$false;$checkpointSave=$null
    $durationWall=$null;$finishElapsedWall=$null;$observationSettleWall=$null;$cameraCohort=$null;$readySeen=$false
    $controlSequences=[Collections.Generic.HashSet[long]]::new()
    $writer=[IO.StreamWriter]::new($OutputPath,$false,[Text.UTF8Encoding]::new($false))
    try{
        if(-not [IO.File]::Exists($LogPath)){$errors.Add('Missing game log')}
        elseif((Get-Item -LiteralPath $LogPath).Length -gt 134217728){$errors.Add('Game log exceeds bounded 128 MiB parser limit')}
        else{foreach($line in [IO.File]::ReadLines($LogPath)){
            $index=$line.IndexOf('OMWROUTE|',[StringComparison]::Ordinal)
            if($index -lt 0){continue}
            if($seq -ge 60000 -or $bytes -gt 16777216){$errors.Add('Route record budget exceeded');break}
            $raw=$line.Substring($index+9)
            if($raw.Length -gt 8192){$errors.Add('Tagged record exceeds 8 KiB');continue}
            try{$record=$raw | ConvertFrom-Json}catch{$errors.Add('Malformed tagged JSON');continue}
            if(-not (Test-OptimizedMWRouteFiniteTree $record)){$errors.Add('Nonfinite or excessively nested record data');continue}
            if($record.schema -ne 1 -or [string]$record.session -cne $SessionId){$errors.Add('Record schema/session mismatch');continue}
            if($record.seq -ne ($seq+1)){$errors.Add('Non-contiguous record sequence')}
            if(-not (Test-OptimizedMWRouteNumber $record.seq 1 60000) -or [double]$record.seq -ne [math]::Floor([double]$record.seq)){$errors.Add('Invalid sequence number');continue}
            $seq=[long]$record.seq
            if(-not (Test-OptimizedMWRouteNumber $record.t 0 86400) -or [double]$record.t -lt $lastTime){$errors.Add('Invalid/non-monotonic record clock')}
            else{$lastTime=[double]$record.t}
            $event=[string]$record.event
            if($event -notin @('ready','start','sample','control','checkpoint','activation','door','trigger','cell','pause','finish','policy','combat','error','checkpoint_save')){$errors.Add('Unknown record event')}
            if(-not $events.ContainsKey($event)){$events[$event]=0};$events[$event]++
            if($finished){$errors.Add('Record emitted after finish')}
            if($event -eq 'ready'){
                $readySeen=$true;$cameraCohort=$record.camera_layer_cohort
                if(-not (Test-OptimizedMWRouteCameraCohort $cameraCohort)){$errors.Add('Invalid camera layer cohort')}
            }
            if($event -in @('start','finish')){
                $other=$record.camera_layer_cohort
                if(-not $readySeen -or -not (Test-OptimizedMWRouteCameraCohort $other) -or ($null -eq $other) -ne ($null -eq $cameraCohort)){$errors.Add('Camera layer boundary cohort mismatch')}
            }
            if($null -ne $cameraCohort){
                $cameraPose=$null
                if($event -in @('ready','checkpoint','finish')){$cameraPose=$record.pose}
                elseif($event -eq 'start'){$cameraPose=$record.checkpoint_pose}
                elseif($event -eq 'sample'){$cameraPose=$record}
                if($event -in @('ready','start','sample','checkpoint','finish') -and -not (Test-OptimizedMWRouteCameraLayers $cameraPose.camera)){$errors.Add('Missing or invalid recorded camera layers')}
            }
            switch($event){
                'start'{if($started){$errors.Add('Duplicate route start')};$started=$true;$startTime=[double]$record.t;if($record.PSObject.Properties['checkpoint_save']){$checkpointSave=$record.checkpoint_save}}
                'checkpoint_save'{if($record.PSObject.Properties['checkpoint_save']){$checkpointSave=$record.checkpoint_save}else{$checkpointSave=$record}}
                'sample'{
                    if(-not $started){$errors.Add('Sample before route start')}
                    foreach($axis in @('x','y','z')){
                        if(-not (Test-OptimizedMWRouteNumber $record.position.$axis)){$errors.Add('Invalid sample position')}
                        if(-not (Test-OptimizedMWRouteNumber $record.rotation.$axis -10000 10000)){$errors.Add('Invalid sample rotation')}
                    }
                    foreach($angle in @('yaw','pitch','extra_yaw','extra_pitch','roll','fov')){if(-not (Test-OptimizedMWRouteNumber $record.camera.$angle -10000 10000)){$errors.Add('Invalid sample camera')}}
                    foreach($clock in @('simulation','wall_since_start','game','dt_real')){if(-not (Test-OptimizedMWRouteNumber $record.$clock 0 86400)){$errors.Add('Invalid sample clock')}}
                    if([double]$record.simulation -lt $lastSimulation){$errors.Add('Sample simulation clock reversed')};$lastSimulation=[double]$record.simulation
                    if(-not (Test-OptimizedMWRouteNumber $record.f 0 60000000) -or [double]$record.f -ne [math]::Floor([double]$record.f) -or [long]$record.f -le $lastFrame){$errors.Add('Invalid or duplicate pose frame')};$lastFrame=[long]$record.f
                    if(-not (Test-OptimizedMWRouteNumber $record.control_sequence 1 60000) -or [double]$record.control_sequence -ne [math]::Floor([double]$record.control_sequence) -or [long]$record.control_sequence -ge $seq -or -not $controlSequences.Contains([long]$record.control_sequence)){$errors.Add('Sample lacks an exact prior control-state join')}
                    if($null -eq $record.cell -or -not [string]$record.cell.name -and -not $record.cell.exterior){$errors.Add('Sample lacks known cell')}else{$cellKnown=$true}
                }
                'control'{
                    if(-not $started){$errors.Add('Control before route start')}
                    foreach($value in @('movement','side_movement','yaw_change','pitch_change')){if(-not (Test-OptimizedMWRouteNumber $record.controls.$value -10000 10000)){$errors.Add('Invalid control-state value')}}
                    foreach($value in @('run','sneak','jump')){if($record.controls.$value -isnot [bool]){$errors.Add('Missing or invalid boolean control-state value')}}
                    if(-not (Test-OptimizedMWRouteNumber $record.controls.use 0 4) -or [double]$record.controls.use -ne [math]::Floor([double]$record.controls.use)){$errors.Add('Invalid native attack/use control enum')}
                    [void]$controlSequences.Add($seq)
                }
                'activation'{
                    if(-not $started){$errors.Add('Activation before route start')}
                    if($record.actual_request -isnot [bool] -or -not $record.actual_request -or $record.observed -isnot [bool] -or -not $record.observed -or [string]$record.source -cne 'engine_onActivate_actual_target'){$errors.Add('Actual activation request was not observed exactly')}
                    if(-not $record.target -or -not [string]$record.target.id -or -not [string]$record.target.record_id){$errors.Add('Actual activation lacks target identity')}
                }
                'door'{if(-not $started){$errors.Add('Door activation before route start')};if(-not [string]$record.id -or -not [string]$record.record_id){$errors.Add('Door activation lacks identity')}}
                'finish'{$finish=$record;$finished=$true;if(-not $started){$errors.Add('Finish before start')}}
                'combat'{$errors.Add('Unexpected combat event')}
                'error'{$errors.Add('Recorder reported an error')}
            }
            $writer.WriteLine($raw);$bytes+=[Text.Encoding]::UTF8.GetByteCount($raw)+1
            if($errors.Count -gt 100){$errors.Add('Too many qualification failures');break}
        }}
    }finally{$writer.Dispose()}
    if(-not $started){$errors.Add('Missing route start')}
    if(-not $events.ContainsKey('ready') -or $events['ready'] -ne 1){$errors.Add('Missing or duplicate ready marker')}
    if(-not $finished){$errors.Add('Missing finish/export marker')}
    if(-not $events.ContainsKey('sample') -or $events['sample'] -lt 2){$errors.Add('Insufficient pose samples')}
    if(-not $events.ContainsKey('control') -or $events['control'] -lt 1){$errors.Add('Control-state records missing')}
    if(-not $cellKnown){$errors.Add('No known cell observation')}
    if(-not $checkpointSave -or -not [string]$checkpointSave.directory -or -not [string]$checkpointSave.filename){$errors.Add('Starting checkpoint save missing')}
    if($finished){
        if($lastTime-$startTime -gt 610){$errors.Add('Recording duration exceeds 600-second budget')}
        if($started){$finishElapsedWall=[double]$finish.t-$startTime}
        if(-not (Test-OptimizedMWRouteNumber $finish.duration_wall 0 610)){$errors.Add('Finish wall duration missing or invalid')}
        else{$durationWall=$finish.duration_wall}
        $compatible=$false
        if($finish.PSObject.Properties['capture_scope'] -and $finish.capture_scope.PSObject.Properties['playback_compatible']){
            if($finish.capture_scope.playback_compatible -isnot [bool]){$errors.Add('Finish playback-compatible scope must be boolean')}
            else{$compatible=$finish.capture_scope.playback_compatible}
        }
        if($finish.PSObject.Properties['observation_settle_wall']){
            if(-not (Test-OptimizedMWRouteNumber $finish.observation_settle_wall 0 86400)){$errors.Add('Finish observation-settle wall duration invalid')}
            else{$observationSettleWall=$finish.observation_settle_wall}
        }
        if($compatible){
            if(-not (Test-OptimizedMWRouteNumber $finish.acquisition_end_wall 0 610)){$errors.Add('Compatible acquisition-end wall duration missing or invalid')}
            elseif($null -ne $durationWall -and [Math]::Abs([double]$finish.acquisition_end_wall-[double]$durationWall) -gt 0.000001){$errors.Add('Compatible acquisition-end/footer wall duration mismatch')}
            if($null -eq $observationSettleWall){$errors.Add('Compatible observation-settle wall duration missing or invalid')}
        }
        # Compatible duration stops at F10 before the independent observer drain.
        # Legacy footer durations include their drain. Copy the footer value exactly
        # for the compiler join; use a tolerance only for arithmetic clock closure.
        if($started -and $null -ne $durationWall -and (-not $compatible -or $null -ne $observationSettleWall)){
            $expectedElapsed=[double]$durationWall
            if($compatible){$expectedElapsed+=[double]$observationSettleWall}
            if([Math]::Abs([double]$finishElapsedWall-$expectedElapsed) -gt 0.000001){$errors.Add('Finish elapsed/acquisition/drain wall clock mismatch')}
        }
        if(-not $finish.PSObject.Properties['expected_records'] -or [long]$finish.expected_records -ne $seq){$errors.Add('Finish record-count mismatch')}
        if($finish.PSObject.Properties['complete'] -and -not [bool]$finish.complete){$errors.Add('Recorder marks route incomplete')}
        if(-not $finish.PSObject.Properties['flags'] -or $finish.flags.complete -isnot [bool] -or -not $finish.flags.complete){$errors.Add('Recorder flags route incomplete')}
        if($finish.flags.door_observer_ready -isnot [bool] -or -not $finish.flags.door_observer_ready){$errors.Add('Door observer unavailable')}
        if($finish.flags.policy_ready -isnot [bool] -or -not $finish.flags.policy_ready -or $finish.flags.policy_qualified -isnot [bool] -or -not $finish.flags.policy_qualified){$errors.Add('Actor policy unavailable or unqualified')}
        foreach($badFlag in @('combat_invalid','reloaded','budget_exceeded','checkpoint_failed','lost_buffer','nonfinite_data','observation_settle_timeout','activation_target_unresolved','vanity_setup_failed')){
            if($finish.flags.PSObject.Properties[$badFlag] -and ($finish.flags.$badFlag -isnot [bool] -or $finish.flags.$badFlag)){$errors.Add('Recorder failure flag: '+$badFlag)}
        }
        if(-not $finish.PSObject.Properties['policy']){$errors.Add('Actor policy qualification metadata missing')}
        else{
            foreach($requiredPolicy in @('qualified','ready','enabled')){if($finish.policy.$requiredPolicy -isnot [bool] -or -not $finish.policy.$requiredPolicy){$errors.Add('Actor policy metadata not qualified')}}
            if($finish.policy.failed -isnot [bool] -or $finish.policy.failed){$errors.Add('Actor policy reports a failure')}
            foreach($zeroCounter in @('combat_during_recording','pending_disable')){
                $value=$finish.policy.$zeroCounter
                if(-not (Test-OptimizedMWRouteNumber $value 0 100000000) -or [Math]::Floor([double]$value) -ne [double]$value){$errors.Add('Actor policy counter missing or invalid: '+$zeroCounter)}
                elseif($value -gt 0){
                    if($zeroCounter -eq 'combat_during_recording'){$errors.Add('Combat/Pursue was observed during recording (events='+$value+')')}
                    else{$errors.Add('Actor suppressions still pending at export (actors='+$value+')')}
                }
            }
        }
        if(-not $finish.PSObject.Properties['private_camera_setup'] -or [string]$finish.private_camera_setup.source -cne 'private_setup_vanity_control_switch' -or $finish.private_camera_setup.verified -isnot [bool] -or -not $finish.private_camera_setup.verified -or $finish.private_camera_setup.vanity_disabled -isnot [bool] -or -not $finish.private_camera_setup.vanity_disabled){$errors.Add('Private idle-camera setup missing or unverified')}
        if($finish.PSObject.Properties['reason'] -and [string]$finish.reason -notin @('user_finished','smoke_finished')){$errors.Add('Recorder finish reason is incomplete')}
        if(-not $finish.PSObject.Properties['counts']){$errors.Add('Finish event counts missing')}
        else{
            foreach($name in $events.Keys){if($name -eq 'finish'){continue};if(-not $finish.counts.PSObject.Properties[$name] -or [int]$finish.counts.$name -ne $events[$name]){$errors.Add('Finish event count mismatch')}}
            foreach($property in $finish.counts.PSObject.Properties){if(-not $events.ContainsKey($property.Name)){$errors.Add('Finish counts include an unobserved event')}}
        }
    }
    if($null -eq $ExitCode -or $ExitCode -ne 0){$errors.Add('Game did not exit normally')}
    return [pscustomobject][ordered]@{schema=1;session=$SessionId;complete=($errors.Count -eq 0);qualification=$(if($errors.Count -eq 0){'RECORDED_NOT_REPLAY_VALIDATED'}else{'INCOMPLETE'});
        record_count=$seq;events=$events;duration_seconds=$durationWall;duration_seconds_source='finish.duration_wall';
        finish_elapsed_wall_seconds=$finishElapsedWall;observation_settle_wall_seconds=$observationSettleWall;
        finish_reason=$(if($finish){[string]$finish.reason}else{$null});camera_layer_cohort=$cameraCohort;camera_guard=$(if($finish){$finish.camera_guard}else{$null});errors=@($errors | Select-Object -Unique);
        route_file=(Get-OptimizedMWFrozenFile $OutputPath);source_log=$LogPath;exit_code=$ExitCode;checkpoint_save=$checkpointSave;
        limitation='Recording integrity is distinct from exact replay, actor repeatability, candidate engagement, and runtime performance acceptance.'}
}

function Copy-OptimizedMWRouteStartSave {
    param([object]$Summary,[object]$Session)
    $checkpoint=$Summary.checkpoint_save
    if(-not $checkpoint){throw 'Route starting checkpoint metadata missing'}
    $expectedDescription='OptimizedMW route '+[string]$Session.manifest.route+' '+[string]$Session.id
    $requestPattern='^'+[regex]::Escape($Session.id)+':start(?::(?:[2-9]|[1-9][0-9]{1,5}))?$'
    if([string]$checkpoint.source -cne 'private_menu_new_save_slot' -or [string]$checkpoint.request -cnotmatch $requestPattern -or [string]$checkpoint.description -cne $expectedDescription){throw 'Starting checkpoint is not the authenticated new private route slot'}
    $directory=[string]$checkpoint.directory;$filename=[string]$checkpoint.filename
    foreach($part in @($directory,$filename)){
        if(-not $part -or $part.Length -gt 256 -or $part -match '[\\/:\r\n]|^\.{1,2}$|[. ]$' -or [IO.Path]::IsPathRooted($part)){throw 'Unsafe checkpoint save identity'}
    }
    if($filename -notmatch '\.omwsave$'){throw 'Checkpoint is not an omwsave'}
    $saveRoot=Assert-OptimizedMWPlainPath (Join-Path $Session.runtime.user_data_directory 'saves')
    $source=Assert-OptimizedMWPlainPath (Join-Path (Join-Path $saveRoot $directory) $filename)
    if(-not $source.StartsWith($saveRoot.TrimEnd('\','/')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Checkpoint escapes private saves'}
    if($source -ieq [IO.Path]::GetFullPath($Session.runtime.save_path)){throw 'Initial copied save is not a newly captured starting checkpoint'}
    $identity=Get-OptimizedMWFrozenFile $source
    if($identity.bytes -lt 1 -or $identity.bytes -gt 134217728){throw 'Checkpoint save exceeds 128 MiB budget or is empty'}
    $target=Assert-OptimizedMWPlainPath (Join-Path $Session.directory 'Start.omwsave')
    [IO.File]::Copy($source,$target,$false)
    $copy=Get-OptimizedMWFrozenFile $target
    if($copy.sha256 -cne $identity.sha256 -or $copy.bytes -ne $identity.bytes){throw 'Starting save copy mismatch'}
    return [pscustomobject]@{source=$identity;route_copy=$copy;metadata=$checkpoint;scope='Private starting world save; exact subsequent replay still requires validation'}
}

function Start-OptimizedMWRouteOwnedProcess {
    param([string]$Executable,[string[]]$Arguments,[string]$WorkingDirectory,[string]$StandardOutput,[string]$StandardError)
    $process=Start-Process -FilePath $Executable -ArgumentList $Arguments -WorkingDirectory $WorkingDirectory -RedirectStandardOutput $StandardOutput -RedirectStandardError $StandardError -PassThru
    # Windows PowerShell Start-Process can return an object whose exit code is
    # not retained unless its process handle is opened while the child exists.
    # Cache that actual OS handle now; Refresh after exit would discard cached
    # process state. Never infer a zero exit code from a Lua footer or log text.
    $ownedHandle=$process.Handle
    if($ownedHandle -eq [IntPtr]::Zero){throw 'Cannot retain the launched process handle for OS exit qualification'}
    return [pscustomobject]@{process=$process;handle=$ownedHandle}
}

function Invoke-OptimizedMWRouteRecorder {
    $game=[IO.Path]::GetFullPath($GameDir)
    . (Join-Path $game 'OptimizedMW_FrozenConfig.ps1')
    $game=Assert-OptimizedMWPlainPath $game
    $package=$RecorderPackageDirectory;if(-not $package){$package=Join-Path $game 'RouteRecorder'}
    $choice=$Route
    if(-not $choice){
        Write-Host '1 = Record Seyda Neen route';Write-Host '2 = Record Balmora route';Write-Host '0 = Exit'
        do{$number=Read-Host 'Route (0-2)'}while($number -notin @('0','1','2'))
        if($number -eq '0'){return}
        $choice=$(if($number -eq '1'){'existing'}else{'balmora'})
    }
    $frozen=$FrozenConfigDirectory
    if(-not $frozen){$frozen=Join-Path $game $(if($choice -eq 'balmora'){'Configuration\Benchmark-SaintJs-Balmora'}else{'Configuration\Benchmark-SaintJs'})}
    if(@(Get-Process -Name openmw -ErrorAction SilentlyContinue).Count){throw 'Close the existing OpenMW instance before preparing a route recording'}
    $session=New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $frozen -PackageDirectory $package -RouteName $choice -SmokeRun:$Smoke -SmokeDuration $SmokeSeconds -SmallValidationWindow:$ValidationWindow -PlaybackCompatible:(-not $Smoke) -NativeMovement:(-not $AnimationMovement)
    Write-Host ('Private recording profile: '+$session.runtime.directory)
    Write-Host ('Route output: '+$session.directory)
    Write-Host ('Private encounter policy: '+$session.manifest.encounter_policy.explicit_record_count+' creature records; ordinary NPCs retained.')
    if($choice -eq 'existing'){Write-Host 'Walk outside the Census office BEFORE F6. Choose the usual outdoor starting view.'}
    else{Write-Host 'Walk outside Caius Cosades House BEFORE F6. Choose the usual outdoor starting view.'}
    Write-Host ('Private movement cohort: '+$session.manifest.movement_cohort.model+'; visual animations remain enabled.')
    Write-Host 'Recording waits for an outdoor first-person view, collision and hostile-policy readiness.'
    Write-Host 'F6 starts; F9 marks a numbered stage; F10 finishes and exits; F11 aborts and exits. Press these keys alone; no Ctrl or Shift.'
    Write-Host 'Hold the fixed view for 45 seconds after Recording appears, press F9, then walk the outdoor route. Autorun and gait toggles are supported; avoid doors, menus, combat and jump.'
    Write-Host 'Mod camera rotation and first-person offsets are recorded explicitly; normal fatigue sway and head bobbing are supported.'
    Write-Host 'Press F10 before 180 simulation seconds total (including the fixed view). This is route setup, not a performance capture.'
    if($PrepareOnly){Write-Host 'Prepared only; no game launched.';return $session}
    Assert-OptimizedMWRouteSessionInputs $session
    $envBefore=@{};$observerNames=@(Get-ChildItem Env: | Where-Object {$_.Name -like 'OPENMW_*'})
    foreach($entry in $observerNames){$envBefore[$entry.Name]=$entry.Value;[Environment]::SetEnvironmentVariable($entry.Name,$null,'Process')}
    $process=$null
    try{
        $ownedProcess=Start-OptimizedMWRouteOwnedProcess -Executable $session.executable -Arguments $session.arguments -WorkingDirectory $game -StandardOutput $session.manifest.stdout_path -StandardError $session.manifest.stderr_path
        $process=$ownedProcess.process
        $session.manifest | Add-Member -NotePropertyName game_pid -NotePropertyValue $process.Id -Force
        Write-OptimizedMWRouteJson (Join-Path $session.directory 'SESSION.json') $session.manifest
        # Lua controls graceful smoke completion. The host never force-kills a
        # game or claims an unfinished recording succeeded.
        if($Smoke){
            $watch=[Diagnostics.Stopwatch]::StartNew();$nextProgress=30
            while(-not $process.WaitForExit(1000)){
                if($watch.Elapsed.TotalSeconds -ge 300){
                    Write-OptimizedMWRouteJson (Join-Path $session.directory 'SUMMARY.json') ([pscustomobject]@{schema=1;complete=$false;qualification='SMOKE_TIMEOUT_GAME_LEFT_RUNNING';game_pid=$process.Id;directory=$session.directory})
                    throw 'Smoke did not finish within 300 seconds; the private game remains running for inspection'
                }
                if($watch.Elapsed.TotalSeconds -ge $nextProgress){Write-Host ('Waiting for private smoke setup/recording/export; elapsed '+[int]$watch.Elapsed.TotalSeconds+' seconds. Compatibility dialogs may need acknowledgment.');$nextProgress+=30}
            }
        }
        if(-not $Smoke){$process.WaitForExit()}
        $exitCode=$process.ExitCode
        $logs=@($session.manifest.stdout_path,(Join-Path $session.runtime.directory 'openmw.log'),(Join-Path $session.runtime.user_data_directory 'openmw.log'))
        $sourceLog=$logs[0]
        foreach($log in $logs){if([IO.File]::Exists($log) -and (Select-String -LiteralPath $log -Pattern 'OMWROUTE\|' -Quiet)){$sourceLog=$log;break}}
        $summary=Export-OptimizedMWRouteLog -LogPath $sourceLog -OutputPath (Join-Path $session.directory 'route.jsonl') -SessionId $session.id -ExitCode $exitCode
        try{$startSave=Copy-OptimizedMWRouteStartSave $summary $session;$summary | Add-Member -NotePropertyName starting_save -NotePropertyValue $startSave}
        catch{$summary.complete=$false;$summary.qualification='INCOMPLETE';$summary.errors+=@('Starting checkpoint save not qualified: '+$_.Exception.Message)}
        $after=Get-OptimizedMWRouteProtectedInputs $session.baseline
        $summary | Add-Member -NotePropertyName protected_after -NotePropertyValue $after -Force
        $unchanged=Compare-OptimizedMWRouteProtectedInputs $session.manifest.protected_before $after
        $summary | Add-Member -NotePropertyName protected_inputs_unchanged -NotePropertyValue $unchanged -Force
        if(-not $unchanged){$summary.complete=$false;$summary.qualification='INCOMPLETE';$summary.errors+=@('Protected gameplay inputs changed during recording')}
        Write-OptimizedMWRouteJson (Join-Path $session.directory 'SUMMARY.json') $summary
        if(-not $summary.complete){Write-Host ('Recording incomplete: '+($summary.errors -join '; ')) -ForegroundColor Yellow}
        else{Write-Host 'Route recorded. Replay validation is still required.' -ForegroundColor Green}
        return $summary
    }finally{foreach($name in $envBefore.Keys){[Environment]::SetEnvironmentVariable($name,$envBefore[$name],'Process')}}
}

if(-not $LibraryOnly){Invoke-OptimizedMWRouteRecorder}
