param(
    [string]$GameDir=$PSScriptRoot,
    [string]$CaptureDirectory='',
    [ValidateSet('','existing','balmora')][string]$Route='',
    [string]$FrozenConfigDirectory='',
    [string]$PythonExecutable='',
    [switch]$RawInputs,
    [switch]$AnimationMovement,
    [switch]$PrepareOnly,
    [switch]$LibraryOnly
)
$ErrorActionPreference='Stop'
# Dot-sourced recorder parameters must not replace this invocation's switches.
$playbackInvocation=@{Game=$GameDir;Capture=$CaptureDirectory;Route=$Route;Frozen=$FrozenConfigDirectory;Python=$PythonExecutable;Raw=[bool]$RawInputs;Animation=[bool]$AnimationMovement;Prepare=[bool]$PrepareOnly;Library=[bool]$LibraryOnly}
. (Join-Path $PSScriptRoot 'OptimizedMW_FrozenConfig.ps1')
. (Join-Path $PSScriptRoot 'OptimizedMW_RouteRecorder.ps1') -GameDir $playbackInvocation.Game -LibraryOnly

function Assert-OptimizedMWPlaybackIdentities {
    param([object[]]$Identities)
    foreach($identity in $Identities){
        $actual=Get-OptimizedMWFrozenFile $identity.path
        if($actual.sha256 -cne $identity.sha256 -or $actual.bytes -ne $identity.bytes){throw ('Playback input changed: '+$identity.path)}
    }
}

function Read-OptimizedMWPlaybackCapture {
    param([string]$Directory,[ValidateSet('','existing','balmora')][string]$ExpectedRoute='')
    $capture=Assert-OptimizedMWPlainPath $Directory
    $sessionIdentity=Get-OptimizedMWFrozenFile (Join-Path $capture 'SESSION.json')
    $summaryIdentity=Get-OptimizedMWFrozenFile (Join-Path $capture 'SUMMARY.json')
    if($sessionIdentity.bytes -gt 8388608 -or $summaryIdentity.bytes -gt 1048576){throw 'Capture admission metadata exceeds its budget'}
    $source=[IO.File]::ReadAllText($sessionIdentity.path) | ConvertFrom-Json
    $status=[IO.File]::ReadAllText($summaryIdentity.path) | ConvertFrom-Json
    if($source.schema -ne 1 -or [string]$source.route -cnotin @('existing','balmora') -or [string]$source.session -cnotmatch '^[a-f0-9]{32}$'){throw 'Capture route/session metadata is invalid'}
    if($ExpectedRoute -and [string]$source.route -cne $ExpectedRoute){throw 'Selected capture belongs to a different route'}
    if($status.complete -isnot [bool] -or -not $status.complete -or [string]$status.qualification -cne 'RECORDED_NOT_REPLAY_VALIDATED' -or [string]$status.session -cne [string]$source.session){throw 'Capture is incomplete or does not have a qualified recording footer; record a fresh outdoor route'}
    $cohort=$null
    if($source.PSObject.Properties['movement_cohort']){$cohort=$source.movement_cohort}
    if($null -ne $cohort -and ($cohort.private_override -isnot [bool] -or [string]$cohort.model -cnotin @('native_input_displacement_with_visual_animations','animation_displacement') -or ($cohort.private_override -ne ([string]$cohort.model -ceq 'native_input_displacement_with_visual_animations')))){throw 'Capture movement cohort metadata is invalid'}
    if([string]$source.route -ceq 'existing' -and ($null -eq $cohort -or [string]$cohort.model -cne 'native_input_displacement_with_visual_animations' -or -not $cohort.private_override)){throw 'Seyda Neen requires a fresh outdoor recording made with the matching native movement profile'}
    $cameraCohort=$source.camera_layer_cohort;$summaryCamera=$status.camera_layer_cohort
    if(-not (Test-OptimizedMWRouteCameraCohort $cameraCohort) -or -not (Test-OptimizedMWRouteCameraCohort $summaryCamera) -or
        ($null -eq $cameraCohort) -ne ($null -eq $summaryCamera)){throw 'Capture camera layer cohort is invalid or differs between session and summary'}
    Assert-OptimizedMWPlaybackIdentities @($sessionIdentity,$summaryIdentity)
    return [pscustomobject]@{directory=$capture;route=[string]$source.route;session=[string]$source.session;movement_cohort=$cohort;camera_layer_cohort=$cameraCohort;metadata_identities=@($sessionIdentity,$summaryIdentity)}
}

function Assert-OptimizedMWPlaybackCameraReceipt {
    param($Source,$Receipt)
    $sourceCamera=$Source.camera_layer_cohort;$compiledCamera=$Receipt.camera_layer_cohort
    $expectedStatus=$(if($null -ne $sourceCamera){'recorded_additive'}else{'legacy_zero_layers'})
    if(-not (Test-OptimizedMWRouteCameraCohort $sourceCamera) -or -not (Test-OptimizedMWRouteCameraCohort $compiledCamera) -or
        ($null -eq $sourceCamera) -ne ($null -eq $compiledCamera) -or [string]$Receipt.camera_layer_cohort_status -cne $expectedStatus){
        throw 'Compiled camera layer contract differs from the admitted source'
    }
}

function New-OptimizedMWPlaybackSession {
    param([string]$GameDirectory,[string]$Capture,[string]$Frozen,[string]$Python,[switch]$Raw,[switch]$Animation,[ValidateSet('','existing','balmora')][string]$RouteName='')
    $game=Assert-OptimizedMWPlainPath $GameDirectory
    # Refuse old/incomplete sources before creating a private session.
    $source=Read-OptimizedMWPlaybackCapture -Directory $Capture -ExpectedRoute $RouteName
    $capture=$source.directory;$RouteName=$source.route
    if(-not $Frozen){$Frozen=Join-Path $game $(if($RouteName -ceq 'balmora'){'Configuration\Benchmark-SaintJs-Balmora'}else{'Configuration\Benchmark-SaintJs'})}
    if(-not $Python){$Python=(Get-Command python.exe -ErrorAction Stop).Source}
    $pythonIdentity=Get-OptimizedMWFrozenFile $Python
    $package=Assert-OptimizedMWPlainPath (Join-Path $game 'RoutePlayback')
    $session=New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $Frozen -PackageDirectory (Join-Path $game 'RouteRecorder') -RouteName $RouteName -SmallValidationWindow -OutputCategory Playback
    # Guided feedback needs the native input vector to determine displacement.
    # Keep authored animation displacement for the raw control/explicit experiment.
    $sourceNative=($null -ne $source.movement_cohort -and $source.movement_cohort.private_override)
    $nativeMovement=(-not $Animation -and (-not $Raw -or $sourceNative))
    if($nativeMovement -or $Animation){Set-OptimizedMWRouteIniValue -Path $session.runtime.settings_path -Section 'Game' -Key 'player movement ignores animation' -Value $(if($nativeMovement){'true'}else{'false'})}
    $data=Join-Path $session.runtime.user_data_directory 'data'
    $relativeFiles=@('optimizedmw_route_playback.omwscripts','scripts\optimizedmw_route_playback\core.lua','scripts\optimizedmw_route_playback\player.lua','scripts\optimizedmw_route_playback\guidance.lua')
    $playbackInputs=[Collections.Generic.List[object]]::new()
    foreach($relative in $relativeFiles){
        $packageSource=Get-OptimizedMWFrozenFile (Join-Path $package $relative)
        $target=Assert-OptimizedMWPlainPath (Join-Path $data $relative)
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
        [IO.File]::Copy($packageSource.path,$target,$false)
        $copied=Get-OptimizedMWFrozenFile $target
        if($packageSource.sha256 -cne $copied.sha256 -or $packageSource.bytes -ne $copied.bytes){throw 'Playback package copy mismatch'}
        $playbackInputs.Add($packageSource);$playbackInputs.Add($copied)
    }
    $compiler=Get-OptimizedMWFrozenFile (Join-Path $package 'compile_route.py')
    $timeline=Join-Path $data 'scripts\optimizedmw_route_playback\timeline.lua'
    $receiptPath=Join-Path $session.directory 'COMPILED.json'
    $compilerOutput=@(& $Python $compiler.path --capture $capture --output $timeline --receipt $receiptPath)
    if($LASTEXITCODE -ne 0){throw 'Playback compiler refused the capture'}
    Write-Host ($compilerOutput -join "`n")
    $receipt=[IO.File]::ReadAllText($receiptPath) | ConvertFrom-Json
    if($receipt.qualification -cne 'COMPILED_NOT_REPLAY_VALIDATED' -or -not $receipt.source_evidence_unchanged){throw 'Playback compiler receipt is unqualified'}
    if([string]$receipt.route_name -cne $RouteName){throw 'Compiled route differs from the selected source'}
    Assert-OptimizedMWPlaybackCameraReceipt $source $receipt
    Assert-OptimizedMWPlaybackIdentities $source.metadata_identities
    Assert-OptimizedMWPlaybackIdentities $receipt.inputs_before
    $sourceStart=Get-OptimizedMWFrozenFile (Join-Path $capture 'Start.omwsave')
    $privateStart=Assert-OptimizedMWPlainPath (Join-Path ([IO.Path]::GetDirectoryName($session.runtime.save_path)) 'Playback-Start.omwsave')
    [IO.File]::Copy($sourceStart.path,$privateStart,$false)
    $startIdentity=Get-OptimizedMWFrozenFile $privateStart
    if($sourceStart.sha256 -cne $startIdentity.sha256 -or $sourceStart.bytes -ne $startIdentity.bytes){throw 'Playback starting-save copy mismatch'}
    # Keep the acquisition addon as a save master, with acquisition disabled.
    # Its independent encounter observer remains enabled and gets a fresh session.
    $recorderOptions=$session.manifest.options.path
    $text=[IO.File]::ReadAllText($recorderOptions)
    if(-not $text.StartsWith('return { enabled=true, schema=1,')){throw 'Unexpected recorder options layout'}
    $text='return { enabled=false, schema=1,'+$text.Substring('return { enabled=true, schema=1,'.Length)
    [IO.File]::WriteAllText($recorderOptions,$text,[Text.UTF8Encoding]::new($false))
    $options=Join-Path $data 'scripts\optimizedmw_route_playback\options.lua'
    $guided=$(if($Raw){'false'}else{'true'})
    $cameraOptions=$(if($null -ne $source.camera_layer_cohort){", camera_layer_cohort={model='recorded_additive_first_person_v1'}"}else{''})
    [IO.File]::WriteAllText($options,("return { enabled=true, session='"+$session.id+"', guidance_enabled="+$guided+$cameraOptions+", autostart_seconds=3, quit_on_finish=true, setup_timeout_seconds=90, position_tolerance=128, abort_position_error=2048, max_duration_seconds=195 }"),[Text.UTF8Encoding]::new($false))
    [IO.File]::AppendAllText($session.runtime.config_path,"`ncontent=optimizedmw_route_playback.omwscripts`n",[Text.UTF8Encoding]::new($false))
    $session.arguments=@(Get-OptimizedMWFrozenArguments $session.runtime)+@('--load-savegame',('"'+$privateStart+'"'),'--skip-menu')
    $session.manifest.arguments=$session.arguments
    $session.manifest.options=Get-OptimizedMWFrozenFile $recorderOptions
    $session.manifest.derived_private_inputs=@((Get-OptimizedMWFrozenFile $session.runtime.config_path),(Get-OptimizedMWFrozenFile $session.runtime.settings_path),(Get-OptimizedMWFrozenFile (Join-Path $session.runtime.directory 'input_v3.xml')))
    $session.manifest.mode=$(if($Raw){'private_physics_input_replay_preview'}else{'private_guided_physics_route_preview'})
    $session.manifest.scope='Visible private route preview. Guided controls and any source-clock holds are recorded; no exact timed-input equivalence or performance acceptance.'
    $session.manifest | Add-Member -NotePropertyName playback_protocol -NotePropertyValue $(if($Raw){'raw_physics_input'}else{'physical_xy_follower_v1'})
    $session.manifest | Add-Member -NotePropertyName movement_model -NotePropertyValue $(if($nativeMovement){'native_input_displacement_with_visual_animations'}elseif($Animation){'animation_displacement'}else{'frozen_animation_movement_setting'})
    $session.manifest | Add-Member -NotePropertyName private_movement_override -NotePropertyValue $(if($nativeMovement -or $Animation){[pscustomobject]@{section='Game';key='player movement ignores animation';value=$(if($nativeMovement){'true'}else{'false'});scope='private preview only; source cohort recorded separately'}}else{$null})
    $session.manifest | Add-Member -NotePropertyName source_movement_cohort -NotePropertyValue $source.movement_cohort
    $session.manifest.camera_layer_cohort=$source.camera_layer_cohort
    $session.manifest | Add-Member -NotePropertyName camera_layer_cohort_status -NotePropertyValue $receipt.camera_layer_cohort_status
    $session.manifest | Add-Member -NotePropertyName source_movement_cohort_matches -NotePropertyValue $(if($null -ne $source.movement_cohort){[bool]($nativeMovement -eq $sourceNative)}else{$null})
    $session.manifest.starting_location_mode='load_copied_captured_start_no_teleport'
    $session.manifest.frozen_configuration.mode='verified_baseline_inputs_before_playback_derivation'
    $session.manifest.frozen_configuration.save_load_status='Baseline seed retained privately; separate playback_start copy is loaded by top-level arguments'
    $session.manifest | Add-Member -NotePropertyName playback_start -NotePropertyValue $startIdentity
    $session.manifest | Add-Member -NotePropertyName source_capture -NotePropertyValue $receipt.inputs_before
    $session.manifest | Add-Member -NotePropertyName source_session -NotePropertyValue $receipt.source_session
    $session.manifest | Add-Member -NotePropertyName compiled_route -NotePropertyValue $receipt
    foreach($identity in @($compiler,$pythonIdentity,(Get-OptimizedMWFrozenFile $PSCommandPath),(Get-OptimizedMWFrozenFile $options),(Get-OptimizedMWFrozenFile $timeline),(Get-OptimizedMWFrozenFile $receiptPath),$startIdentity)){$playbackInputs.Add($identity)}
    $session.manifest | Add-Member -NotePropertyName playback_inputs -NotePropertyValue $playbackInputs.ToArray()
    Assert-OptimizedMWRouteSessionInputs $session
    Assert-OptimizedMWPlaybackIdentities $session.manifest.playback_inputs
    Write-OptimizedMWRouteJson (Join-Path $session.directory 'SESSION.json') $session.manifest
    return $session
}

function Export-OptimizedMWPlaybackLog {
    param([string]$LogPath,[string]$OutputPath,[string]$SessionId,[Nullable[int]]$ExitCode)
    if($SessionId -cnotmatch '^[a-f0-9]{32}$'){throw 'Invalid playback session'}
    if(Test-Path -LiteralPath $OutputPath){throw 'Preserving existing playback evidence'}
    if((Get-OptimizedMWFrozenFile $LogPath).bytes -gt 67108864){throw 'Playback stdout exceeds 64 MiB observer budget'}
    $failures=[Collections.Generic.List[string]]::new();$counts=@{};$seq=0L;$finish=$null;$started=$false;$closed=$false;$cameraCohort=$null
    $layerMaximum=0.0;$offsetMaximum=0.0
    $checks=[Collections.Generic.List[object]]::new();$collisionObservations=[Collections.Generic.List[object]]::new()
    $writer=[IO.StreamWriter]::new($OutputPath,$false,[Text.UTF8Encoding]::new($false))
    try{
        foreach($line in [IO.File]::ReadLines($LogPath)){
            $index=$line.IndexOf('OMWPLAY|',[StringComparison]::Ordinal)
            if($index -lt 0){continue}
            $raw=$line.Substring($index+8)
            if($raw.Length -gt 65536 -or $seq -ge 5000){$failures.Add('Playback observer record budget exceeded');break}
            try{$record=$raw | ConvertFrom-Json}catch{$failures.Add('Malformed playback observation');continue}
            if(-not (Test-OptimizedMWRouteFiniteTree $record)){$failures.Add('Nonfinite or unbounded playback observation');continue}
            $seq++
            if($record.schema -ne 1 -or [string]$record.session -cne $SessionId -or $record.seq -ne $seq){$failures.Add('Mixed or discontinuous playback observations')}
            $event=[string]$record.event
            if($seq -eq 1){$cameraCohort=$record.camera_layer_cohort}
            if(-not (Test-OptimizedMWRouteCameraCohort $record.camera_layer_cohort) -or
                ($null -eq $cameraCohort) -ne ($null -eq $record.camera_layer_cohort) -or
                ($null -ne $cameraCohort -and [string]$record.camera_layer_cohort_status -cne 'recorded_additive')){$failures.Add('Playback camera layer contract missing or changed')}
            if($closed){$failures.Add('Observation follows playback footer')}
            if($seq -eq 1 -and $event -cne 'setup'){$failures.Add('Playback setup is not the first observation')}
            if($event -eq 'start'){$started=$true}
            if($event -in @('check','marker','finish','reference_hold','reference_resume') -and -not $started){$failures.Add('Playback work/footer precedes start')}
            if($event -notin @('setup','start','check','marker','finish','error','combat','alignment_check','alignment_finished','reference_hold','reference_resume')){$failures.Add('Unknown playback event')}
            if($event -in @('setup','start','check','finish')){$collisionObservations.Add($record.physical_state)}
            if(-not $counts.ContainsKey($event)){$counts[$event]=0};$counts[$event]++
            if($event -eq 'check'){
                if(-not (Test-OptimizedMWRouteNumber $record.position_error 0 1e8) -or $record.cell_matches -isnot [bool]){$failures.Add('Invalid playback position/cell check')}
                if($null -ne $cameraCohort){
                    if(-not (Test-OptimizedMWRouteCameraLayers $record.command_camera_layers) -or -not (Test-OptimizedMWRouteCameraLayers $record.camera_layers_after_write -Readback)){$failures.Add('Missing or invalid additive camera setter readback')}
                    else{
                        foreach($angle in @('extra_yaw','extra_pitch','roll','extra_roll')){
                            $errorValue=$record.camera_layer_postwrite_errors.$angle
                            $calculated=[Math]::Abs([double]$record.command_camera_layers.$angle-[double]$record.camera_layers_after_write.$angle)
                            if(-not (Test-OptimizedMWRouteNumber $errorValue 0 0.00001) -or [Math]::Abs([double]$errorValue-$calculated) -gt 0.00000001){$failures.Add('Invalid additive camera angle error')}
                            $layerMaximum=[Math]::Max($layerMaximum,$calculated)
                        }
                        $distanceSquared=0.0
                        foreach($axis in @('x','y','z')){$delta=[double]$record.command_camera_layers.first_person_offset.$axis-[double]$record.camera_layers_after_write.first_person_offset.$axis;$distanceSquared+=$delta*$delta}
                        $calculated=[Math]::Sqrt($distanceSquared);$errorValue=$record.first_person_offset_postwrite_error
                        if(-not (Test-OptimizedMWRouteNumber $errorValue 0 0.001) -or [Math]::Abs([double]$errorValue-$calculated) -gt 0.00000001){$failures.Add('Invalid first-person offset setter error')}
                        $offsetMaximum=[Math]::Max($offsetMaximum,$calculated)
                    }
                }
                $checks.Add($record)
            }
            if($event -eq 'finish'){$finish=$record;$closed=$true}
            if($event -in @('error','combat')){$failures.Add('Playback '+$event+' observed')}
            $writer.WriteLine($raw)
        }
    }finally{$writer.Dispose()}
    foreach($event in @('setup','start','finish')){if(-not $counts.ContainsKey($event) -or $counts[$event] -ne 1){$failures.Add('Missing or duplicate '+$event)}}
    if($null -eq $ExitCode -or $ExitCode -ne 0){$failures.Add('Game did not exit normally')}
    if($checks.Count -lt 2){$failures.Add('Insufficient playback path observations')}
    if($finish){
        $protocol='raw_physics_input'
        if($finish.PSObject.Properties['playback_protocol']){$protocol=[string]$finish.playback_protocol}
        if($protocol -cnotin @('raw_physics_input','physical_xy_follower_v1')){$failures.Add('Unknown playback protocol')}
        $routeFinished=$finish.input_replay_finished
        if($finish.PSObject.Properties['route_playback_finished']){$routeFinished=$finish.route_playback_finished}
        if([string]$finish.reason -cne 'route_finished' -or $routeFinished -isnot [bool] -or -not $routeFinished){$failures.Add('Playback did not finish its route')}
        if($protocol -ceq 'physical_xy_follower_v1' -and ($finish.input_replay_finished -isnot [bool] -or $finish.input_replay_finished)){$failures.Add('Guided playback incorrectly claims raw input replay')}
        if($protocol -ceq 'physical_xy_follower_v1'){
            foreach($physical in $collisionObservations){if($null -eq $physical -or $physical.collision_enabled -isnot [bool] -or -not $physical.collision_enabled){$failures.Add('Guided playback collision state unavailable or disabled');break}}
        }
        if($protocol -ceq 'raw_physics_input' -and ($finish.input_replay_finished -isnot [bool] -or -not $finish.input_replay_finished)){$failures.Add('Raw playback did not finish its input replay')}
        if($finish.policy_stop_acknowledged -isnot [bool] -or -not $finish.policy_stop_acknowledged -or
            $finish.policy.qualified -isnot [bool] -or -not $finish.policy.qualified -or $finish.policy.failed -isnot [bool] -or $finish.policy.failed -or
            $finish.policy.ready -isnot [bool] -or -not $finish.policy.ready -or $finish.policy.enabled -isnot [bool] -or -not $finish.policy.enabled -or
            $finish.policy.combat_before_start -ne 0 -or $finish.policy.combat_during_recording -ne 0 -or $finish.policy.pending_disable -ne 0 -or $finish.policy.failure_count -ne 0 -or
            $finish.policy_observer_drained -isnot [bool] -or -not $finish.policy_observer_drained -or $finish.policy_drain_timed_out -isnot [bool] -or $finish.policy_drain_timed_out){$failures.Add('Final actor policy unavailable or unqualified')}
        if($finish.benchmark_qualified -isnot [bool] -or $finish.benchmark_qualified){$failures.Add('Preview incorrectly claims benchmark qualification')}
        if($finish.check_count -ne $checks.Count){$failures.Add('Playback check-count mismatch')}
        if($null -ne $cameraCohort){
            if($finish.camera_layers_restored -isnot [bool] -or -not $finish.camera_layers_restored -or
                $finish.camera_layer_restoration_failed -isnot [bool] -or $finish.camera_layer_restoration_failed){$failures.Add('Owned additive camera state restoration unverified')}
            if(-not (Test-OptimizedMWRouteNumber $finish.maximum_camera_layer_postwrite_error 0 0.00001) -or
                [double]$finish.maximum_camera_layer_postwrite_error+0.00000001 -lt $layerMaximum -or
                -not (Test-OptimizedMWRouteNumber $finish.maximum_first_person_offset_postwrite_error 0 0.001) -or
                [double]$finish.maximum_first_person_offset_postwrite_error+0.00000001 -lt $offsetMaximum){$failures.Add('Additive camera maximum error footer mismatch')}
        }
    }
    if(-not $finish){$protocol='unavailable'}
    return [pscustomobject][ordered]@{schema=1;session=$SessionId;playback_complete=($failures.Count -eq 0);input_replay_complete=($failures.Count -eq 0 -and $protocol -ceq 'raw_physics_input');playback_protocol=$protocol;qualification='PREVIEW_NOT_BENCHMARK_VALIDATED';
        exit_code=$ExitCode;camera_layer_cohort=$cameraCohort;errors=@($failures | Select-Object -Unique);record_count=$seq;events=$counts;finish=$finish;
        playback_file=(Get-OptimizedMWFrozenFile $OutputPath);scope='Physical route preview; steering, reference-clock holds, path errors and cell mismatches require review before automated benchmarking.'}
}

function Invoke-OptimizedMWRoutePlayback {
    $game=Assert-OptimizedMWPlainPath $playbackInvocation.Game
    if(@(Get-Process -Name openmw -ErrorAction SilentlyContinue).Count){throw 'Close the existing OpenMW instance before playback'}
    $capture=$playbackInvocation.Capture
    $route=$playbackInvocation.Route
    if(-not $capture){
        if(-not $route){
            Write-Host '1 = Preview Seyda Neen route';Write-Host '2 = Preview Balmora route';Write-Host '0 = Exit'
            do{$number=Read-Host 'Route (0-2)'}while($number -notin @('0','1','2'))
            if($number -eq '0'){return}
            $route=$(if($number -eq '1'){'existing'}else{'balmora'})
        }
        $candidates=@(Get-ChildItem -LiteralPath (Join-Path $game 'Benchmarks\Routes') -Directory | Where-Object {$_.Name -like ($route+'_*')} | Sort-Object LastWriteTimeUtc -Descending)
        foreach($candidate in $candidates){
            try{$null=Read-OptimizedMWPlaybackCapture -Directory $candidate.FullName -ExpectedRoute $route;$capture=$candidate.FullName;break}catch{continue}
        }
        if(-not $capture){throw 'No compatible complete recording found for this route. Use START-OptimizedMW-RecordRoute.bat to record it outdoors first.'}
    }
    $session=New-OptimizedMWPlaybackSession -GameDirectory $game -Capture $capture -Frozen $playbackInvocation.Frozen -Python $playbackInvocation.Python -Raw:$playbackInvocation.Raw -Animation:$playbackInvocation.Animation -RouteName $route
    Write-Host ('Source recording: '+$capture)
    Write-Host ('Playback evidence: '+$session.directory)
    Write-Host ('Playback protocol: '+$session.manifest.playback_protocol+'; preview only.')
    Write-Host ('Movement model: '+$session.manifest.movement_model+'; private settings identity recorded.')
    Write-Host 'Visible 1280x720 private preview. Playback starts automatically after loading. F11 aborts.'
    if($playbackInvocation.Prepare){Write-Host 'Prepared only; no game launched.';return $session}
    Assert-OptimizedMWRouteSessionInputs $session
    Assert-OptimizedMWPlaybackIdentities $session.manifest.playback_inputs
    Assert-OptimizedMWPlaybackIdentities $session.manifest.source_capture
    $environment=@{}
    foreach($entry in @(Get-ChildItem Env: | Where-Object {$_.Name -like 'OPENMW_*'})){$environment[$entry.Name]=$entry.Value;[Environment]::SetEnvironmentVariable($entry.Name,$null,'Process')}
    try{
        $owned=Start-OptimizedMWRouteOwnedProcess -Executable $session.executable -Arguments $session.arguments -WorkingDirectory $game -StandardOutput $session.manifest.stdout_path -StandardError $session.manifest.stderr_path
        $process=$owned.process
        $session.manifest | Add-Member -NotePropertyName game_pid -NotePropertyValue $process.Id
        Write-OptimizedMWRouteJson (Join-Path $session.directory 'SESSION.json') $session.manifest
        $watch=[Diagnostics.Stopwatch]::StartNew();$lastNotice=0
        while(-not $process.WaitForExit(1000)){
            if($watch.Elapsed.TotalSeconds -ge 300){throw 'Playback exceeded its host deadline; game preserved for inspection. F11 aborts.'}
            if($watch.Elapsed.TotalSeconds - $lastNotice -ge 30){Write-Host ('Playback still running: '+[int]$watch.Elapsed.TotalSeconds+' seconds.');$lastNotice=$watch.Elapsed.TotalSeconds}
        }
        $process.WaitForExit()
        $summary=Export-OptimizedMWPlaybackLog -LogPath $session.manifest.stdout_path -OutputPath (Join-Path $session.directory 'playback.jsonl') -SessionId $session.id -ExitCode $process.ExitCode
        $sourceUnchanged=$true;$unchanged=$true
        try{Assert-OptimizedMWPlaybackIdentities $session.manifest.source_capture}catch{$sourceUnchanged=$false;$summary.errors+=@($_.Exception.Message);$summary.input_replay_complete=$false;$summary.playback_complete=$false}
        try{
            $protectedAfter=Get-OptimizedMWRouteProtectedInputs $session.baseline
            $unchanged=Compare-OptimizedMWRouteProtectedInputs $session.manifest.protected_before $protectedAfter
        }catch{$unchanged=$false;$summary.errors+=@($_.Exception.Message)}
        if(-not $unchanged){$summary.errors+=@('Protected original inputs changed during playback');$summary.input_replay_complete=$false;$summary.playback_complete=$false}
        $summary | Add-Member -NotePropertyName protected_inputs_unchanged -NotePropertyValue $unchanged
        $summary | Add-Member -NotePropertyName source_capture_unchanged -NotePropertyValue $sourceUnchanged
        Write-OptimizedMWRouteJson (Join-Path $session.directory 'SUMMARY.json') $summary
        if(-not $summary.playback_complete){Write-Warning ('Playback incomplete: '+($summary.errors -join '; '))}
        return $summary
    }finally{foreach($name in $environment.Keys){[Environment]::SetEnvironmentVariable($name,$environment[$name],'Process')}}
}
if(-not $playbackInvocation.Library){
    $playbackResult=Invoke-OptimizedMWRoutePlayback
    $playbackResult
    if($null -ne $playbackResult -and -not $playbackInvocation.Prepare -and -not $playbackResult.playback_complete){exit 1}
}
