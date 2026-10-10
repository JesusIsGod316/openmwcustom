# Hermetic playback host/export checks. No game, UI, original saves, or benchmark writes.
$ErrorActionPreference='Stop'
$phaseRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$hostPath=Join-Path $phaseRoot 'OptimizedMW_RoutePlayback.ps1'
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('OptimizedMW Playback Host '+[Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($fixture)
$session=('a'*32)
$checks=0
function Check-Playback {param([bool]$Condition,[string]$Message) if(-not $Condition){throw $Message};$script:checks++}
function Reject-Playback {param([scriptblock]$Action,[string]$Message) $rejected=$false;try{$null=&$Action}catch{$rejected=$true};Check-Playback $rejected $Message}
function New-PlaybackFixturePolicy {
    return [ordered]@{qualified=$true;ready=$true;enabled=$true;failed=$false;combat_before_start=0;combat_during_recording=0;pending_disable=0;failure_count=0}
}
function New-PlaybackFixtureRows {
    return @(
        [ordered]@{event='setup';scope='private_physics_input_replay_preview';route='balmora';teleport_writes=0},
        [ordered]@{event='start';duration_simulation=2;starting_position=@{x=1;y=2;z=3};initial_position_tolerance=128;divergence_abort=2048;policy=(New-PlaybackFixturePolicy);benchmark_qualified=$false},
        [ordered]@{event='check';expected_time=0;actual_position=@{x=1;y=2;z=3};expected_position=@{x=1;y=2;z=3};position_error=0;expected_cell='Esm3ExteriorCell:-2:-2';actual_cell='Esm3ExteriorCell:-2:-2';cell_matches=$true;expected_camera_yaw=0;actual_camera_yaw=0;camera_yaw_error=0;expected_camera_pitch=0;actual_camera_pitch=0;camera_pitch_error=0;sample_interval=0.05},
        [ordered]@{event='check';expected_time=2;actual_position=@{x=2;y=2;z=3};expected_position=@{x=2;y=2;z=3};position_error=0;expected_cell='Esm3ExteriorCell:-2:-2';actual_cell='Esm3ExteriorCell:-2:-2';cell_matches=$true;expected_camera_yaw=0;actual_camera_yaw=0;camera_yaw_error=0;expected_camera_pitch=0;actual_camera_pitch=0;camera_pitch_error=0;sample_interval=0.05},
        [ordered]@{event='finish';reason='route_finished';input_replay_finished=$true;benchmark_qualified=$false;qualification='PREVIEW_NOT_BENCHMARK_VALIDATED';maximum_position_error=0;maximum_camera_yaw_error=0;maximum_camera_pitch_error=0;cell_mismatch_check_count=0;outside_position_tolerance_check_count=0;check_count=2;policy=(New-PlaybackFixturePolicy);policy_stop_acknowledged=$true;policy_observer_drained=$true;policy_drain_timed_out=$false}
    )
}
function New-GuidedPlaybackFixtureRows {
    $rows=New-PlaybackFixtureRows
    $rows[0].scope='private_guided_physics_route_preview'
    foreach($row in $rows){
        $row.playback_protocol='physical_xy_follower_v1'
        $row.physical_state=[ordered]@{collision_enabled=$true;on_ground=$true;swimming=$false;current_speed=120}
    }
    $rows[4].playback_protocol='physical_xy_follower_v1'
    $rows[4].route_playback_finished=$true
    $rows[4].input_replay_finished=$false
    return $rows
}
function New-LayeredPlaybackFixtureRows {
    $rows=New-GuidedPlaybackFixtureRows
    foreach($row in $rows){$row.camera_layer_cohort=[ordered]@{model='recorded_additive_first_person_v1'};$row.camera_layer_cohort_status='recorded_additive'}
    foreach($row in @($rows[2],$rows[3])){
        $row.command_camera_layers=[ordered]@{extra_yaw=0.01;extra_pitch=0.02;roll=0;extra_roll=0.03;first_person_offset=[ordered]@{x=0;y=0;z=6}}
        $row.camera_layers_after_write=[ordered]@{extra_yaw=0.01;extra_pitch=0.02;roll=0;extra_roll=0.03;first_person_offset=[ordered]@{x=0;y=0;z=6}}
        $row.camera_layer_postwrite_errors=[ordered]@{extra_yaw=0;extra_pitch=0;roll=0;extra_roll=0}
        $row.first_person_offset_postwrite_error=0
    }
    $rows[4].maximum_camera_layer_postwrite_error=0;$rows[4].maximum_first_person_offset_postwrite_error=0
    $rows[4].camera_layers_restored=$true;$rows[4].camera_layer_restoration_failed=$false
    return $rows
}
function Export-PlaybackFixture {
    param([object[]]$Rows,[Nullable[int]]$ExitCode=0,[string]$InjectedRaw='', [switch]$PreserveSequence)
    $id=[Guid]::NewGuid().ToString('N')
    $log=Join-Path $fixture ($id+'.log');$output=Join-Path $fixture ($id+'.jsonl')
    $lines=[Collections.Generic.List[string]]::new();$lines.Add('ordinary game log line')
    for($index=0;$index -lt $Rows.Count;++$index){
        $row=$Rows[$index]
        if(-not $PreserveSequence){$row.seq=$index+1}
        if(-not $row.Contains('schema')){$row.schema=1}
        if(-not $row.Contains('session')){$row.session=$session}
        $row.t_simulation=[double]$index;$row.t_wall=[double]$index
        $lines.Add('[Lua] OMWPLAY|'+($row | ConvertTo-Json -Depth 16 -Compress))
        if($index -eq 1 -and $InjectedRaw){$lines.Add('OMWPLAY|'+$InjectedRaw)}
    }
    [IO.File]::WriteAllLines($log,$lines,[Text.UTF8Encoding]::new($false))
    $before=Get-OptimizedMWFrozenFile $log
    $result=Export-OptimizedMWPlaybackLog -LogPath $log -OutputPath $output -SessionId $session -ExitCode $ExitCode
    $after=Get-OptimizedMWFrozenFile $log
    Check-Playback ($before.sha256 -ceq $after.sha256 -and $before.bytes -eq $after.bytes) 'Exporter changed original playback log'
    return $result
}
function Check-IncompletePlayback {
    param([object[]]$Rows,[string]$Message,[Nullable[int]]$ExitCode=0,[string]$InjectedRaw='', [switch]$PreserveSequence)
    $result=Export-PlaybackFixture -Rows $Rows -ExitCode $ExitCode -InjectedRaw $InjectedRaw -PreserveSequence:$PreserveSequence
    Check-Playback (-not $result.playback_complete -and -not $result.input_replay_complete -and $result.errors.Count -gt 0) $Message
    return $result
}
try{
    # Actual dot-source parameter binding must retain playback invocation values.
    . $hostPath -GameDir $fixture -CaptureDirectory (Join-Path $fixture 'capture') -FrozenConfigDirectory (Join-Path $fixture 'frozen') -PythonExecutable (Join-Path $fixture 'python.exe') -RawInputs -AnimationMovement -PrepareOnly -LibraryOnly
    Check-Playback ($playbackInvocation.Prepare -eq $true -and $playbackInvocation.Library -eq $true) 'Dot-sourced recorder replaced playback switches'
    Check-Playback ($playbackInvocation.Game -ceq $fixture -and $playbackInvocation.Capture -ceq (Join-Path $fixture 'capture') -and $playbackInvocation.Frozen -ceq (Join-Path $fixture 'frozen') -and $playbackInvocation.Python -ceq (Join-Path $fixture 'python.exe')) 'Dot-source replaced selected playback inputs'
    Check-Playback ($playbackInvocation.Raw -eq $true) 'Dot-sourced recorder replaced explicit raw-input protocol selection'
    Check-Playback ($playbackInvocation.Animation -eq $true) 'Dot-sourced recorder replaced explicit animation movement control'
    . $hostPath -GameDir $fixture -AnimationMovement -PrepareOnly -LibraryOnly
    Check-Playback ($playbackInvocation.Animation -eq $true -and $playbackInvocation.Raw -eq $false) 'Animation-only control flag did not survive dot sourcing'
    . $hostPath -GameDir $fixture -PrepareOnly -LibraryOnly
    Check-Playback ($playbackInvocation.Animation -eq $false -and $playbackInvocation.Raw -eq $false) 'Default guided movement inherited stale control flags'
    $tokens=$null;$parseErrors=$null;$null=[Management.Automation.Language.Parser]::ParseFile($hostPath,[ref]$tokens,[ref]$parseErrors)
    Check-Playback ($parseErrors.Count -eq 0) 'Playback host source does not parse'

    # Admit route metadata before any session allocation; raw compiler checks follow.
    $captureFixture=Join-Path $fixture 'qualified-route';[void][IO.Directory]::CreateDirectory($captureFixture)
    $sourceManifest=[ordered]@{schema=1;session=$session;route='balmora'}
    $sourceSummary=[ordered]@{schema=1;session=$session;complete=$true;qualification='RECORDED_NOT_REPLAY_VALIDATED'}
    $manifestPath=Join-Path $captureFixture 'SESSION.json';$summaryPath=Join-Path $captureFixture 'SUMMARY.json'
    Write-OptimizedMWRouteJson $manifestPath $sourceManifest;Write-OptimizedMWRouteJson $summaryPath $sourceSummary
    $admitted=Read-OptimizedMWPlaybackCapture -Directory $captureFixture -ExpectedRoute balmora
    Check-Playback ($admitted.route -ceq 'balmora' -and $admitted.movement_cohort -eq $null -and $admitted.metadata_identities.Count -eq 2) 'Legacy complete Balmora preview control was rejected'
    Reject-Playback {Read-OptimizedMWPlaybackCapture -Directory $captureFixture -ExpectedRoute existing} 'Balmora source was accepted as a selected Seyda route'
    $sourceManifest.route='existing';Write-OptimizedMWRouteJson $manifestPath $sourceManifest
    Reject-Playback {Read-OptimizedMWPlaybackCapture -Directory $captureFixture} 'Legacy Seyda source without matching native movement evidence accepted'
    $sourceManifest.movement_cohort=[ordered]@{model='native_input_displacement_with_visual_animations';private_override=$true};Write-OptimizedMWRouteJson $manifestPath $sourceManifest
    $admitted=Read-OptimizedMWPlaybackCapture -Directory $captureFixture -ExpectedRoute existing
    Check-Playback ($admitted.route -ceq 'existing' -and $admitted.movement_cohort.private_override -eq $true) 'Fresh native Seyda route rejected'
    Check-Playback ($null -eq $admitted.camera_layer_cohort) 'Old zero-layer capture silently changed camera cohort'
    Assert-OptimizedMWPlaybackCameraReceipt $admitted ([pscustomobject]@{camera_layer_cohort=$null;camera_layer_cohort_status='legacy_zero_layers'})
    Check-Playback $true 'Legacy compiler receipt rejected'
    $cameraCohort=[ordered]@{model='recorded_additive_first_person_v1'}
    $sourceManifest.camera_layer_cohort=$cameraCohort;Write-OptimizedMWRouteJson $manifestPath $sourceManifest
    Reject-Playback {Read-OptimizedMWPlaybackCapture -Directory $captureFixture} 'Missing summary camera cohort accepted'
    $sourceSummary.camera_layer_cohort=$cameraCohort;Write-OptimizedMWRouteJson $summaryPath $sourceSummary
    $cameraAdmitted=Read-OptimizedMWPlaybackCapture -Directory $captureFixture
    Check-Playback ($cameraAdmitted.camera_layer_cohort.model -ceq 'recorded_additive_first_person_v1') 'Matching additive camera contract rejected'
    Assert-OptimizedMWPlaybackCameraReceipt $cameraAdmitted ([pscustomobject]@{camera_layer_cohort=[pscustomobject]$cameraCohort;camera_layer_cohort_status='recorded_additive'})
    Check-Playback $true 'Matching additive compiler receipt rejected'
    foreach($badCohort in @([ordered]@{model='unknown'},[ordered]@{model='recorded_additive_first_person_v1';extra=$true},'recorded_additive_first_person_v1')){
        $sourceManifest.camera_layer_cohort=$badCohort;Write-OptimizedMWRouteJson $manifestPath $sourceManifest
        Reject-Playback {Read-OptimizedMWPlaybackCapture -Directory $captureFixture} 'Invalid or expanded source camera contract accepted'
    }
    $sourceManifest.camera_layer_cohort=$cameraCohort;Write-OptimizedMWRouteJson $manifestPath $sourceManifest
    Reject-Playback {Assert-OptimizedMWPlaybackCameraReceipt $cameraAdmitted ([pscustomobject]@{camera_layer_cohort=$null;camera_layer_cohort_status='legacy_zero_layers'})} 'Compiler silently discarded additive source contract'
    Reject-Playback {Assert-OptimizedMWPlaybackCameraReceipt $admitted ([pscustomobject]@{camera_layer_cohort=[pscustomobject]$cameraCohort;camera_layer_cohort_status='recorded_additive'})} 'Compiler silently upgraded legacy source'
    Reject-Playback {Assert-OptimizedMWPlaybackCameraReceipt $cameraAdmitted ([pscustomobject]@{camera_layer_cohort=[pscustomobject]$cameraCohort;camera_layer_cohort_status='legacy_zero_layers'})} 'Compiler camera cohort status mismatch accepted'
    $sourceManifest.Remove('camera_layer_cohort');$sourceSummary.Remove('camera_layer_cohort')
    Write-OptimizedMWRouteJson $manifestPath $sourceManifest;Write-OptimizedMWRouteJson $summaryPath $sourceSummary
    $sourceManifest.movement_cohort.private_override='true';Write-OptimizedMWRouteJson $manifestPath $sourceManifest
    Reject-Playback {Read-OptimizedMWPlaybackCapture -Directory $captureFixture} 'String movement override flag accepted'
    $sourceManifest.movement_cohort.private_override=$false;Write-OptimizedMWRouteJson $manifestPath $sourceManifest
    Reject-Playback {Read-OptimizedMWPlaybackCapture -Directory $captureFixture} 'Native model with disabled native override accepted'
    $sourceManifest.movement_cohort=[ordered]@{model='animation_displacement';private_override=$false};Write-OptimizedMWRouteJson $manifestPath $sourceManifest
    Reject-Playback {Read-OptimizedMWPlaybackCapture -Directory $captureFixture} 'Seyda animation-dependent diagnostic control admitted to default replay'
    $sourceManifest.movement_cohort=[ordered]@{model='native_input_displacement_with_visual_animations';private_override=$true};Write-OptimizedMWRouteJson $manifestPath $sourceManifest
    foreach($badComplete in @($false,'true',$null)){
        $sourceSummary.complete=$badComplete;Write-OptimizedMWRouteJson $summaryPath $sourceSummary
        Reject-Playback {Read-OptimizedMWPlaybackCapture -Directory $captureFixture} 'Incomplete or non-boolean recording admission accepted'
    }
    $sourceSummary.complete=$true;$sourceSummary.session=('b'*32);Write-OptimizedMWRouteJson $summaryPath $sourceSummary
    Reject-Playback {Read-OptimizedMWPlaybackCapture -Directory $captureFixture} 'Mixed session summary admitted'
    $sourceSummary.session=$session;$sourceSummary.qualification='INCOMPLETE';Write-OptimizedMWRouteJson $summaryPath $sourceSummary
    Reject-Playback {Read-OptimizedMWPlaybackCapture -Directory $captureFixture} 'Incomplete qualification admitted despite complete flag'
    . $hostPath -GameDir $fixture -Route existing -PrepareOnly -LibraryOnly
    Check-Playback ($playbackInvocation.Route -ceq 'existing') 'Dot-sourced recorder replaced selected playback route'

    $layered=Export-PlaybackFixture (New-LayeredPlaybackFixtureRows)
    Check-Playback ($layered.playback_complete -and $layered.camera_layer_cohort.model -ceq 'recorded_additive_first_person_v1') 'Qualified sampled additive camera playback rejected'
    $rounded=New-LayeredPlaybackFixtureRows
    $rounded[2].command_camera_layers.extra_pitch=[Math]::PI;$rounded[2].camera_layers_after_write.extra_pitch=3.1415927410125732
    $rounded[2].camera_layer_postwrite_errors.extra_pitch=[Math]::Abs([Math]::PI-3.1415927410125732)
    $rounded[4].maximum_camera_layer_postwrite_error=$rounded[2].camera_layer_postwrite_errors.extra_pitch
    Check-Playback ((Export-PlaybackFixture $rounded).playback_complete) 'Native float rounding at pi was rejected as an invalid readback'
    $rounded[2].command_camera_layers.extra_pitch=3.1415927410125732
    $null=Check-IncompletePlayback $rounded 'Readback slack leaked into strict compiled command admission'
    foreach($mutation in @('missing_contract','missing_readback','wrong_angle_error','wrong_offset_error','wrong_maximum','missing_restore','failed_restore')){
        $rows=New-LayeredPlaybackFixtureRows
        switch($mutation){
            'missing_contract'{$rows[2].Remove('camera_layer_cohort')}
            'missing_readback'{$rows[2].Remove('camera_layers_after_write')}
            'wrong_angle_error'{$rows[2].camera_layers_after_write.extra_pitch=0.021}
            'wrong_offset_error'{$rows[2].camera_layers_after_write.first_person_offset.z=6.0001}
            'wrong_maximum'{$rows[4].maximum_camera_layer_postwrite_error=0.00002}
            'missing_restore'{$rows[4].Remove('camera_layers_restored')}
            'failed_restore'{$rows[4].camera_layer_restoration_failed=$true}
        }
        $null=Check-IncompletePlayback $rows ('Additive camera failure accepted: '+$mutation)
    }

    # Derive the movement setting in a private copy and retain the original cohort.
    $baselineSettings=Join-Path $fixture 'frozen-baseline-settings.cfg'
    $privateSettings=Join-Path $fixture 'derived-private-settings.cfg'
    $settingsText=@'
[Video]
fullscreen = false
[Game]
# keep this comment
player movement ignores animation = false
PLAYER MOVEMENT IGNORES ANIMATION = false
smooth movement = true
[Camera]
player movement ignores animation = false
[game]
player movement ignores animation = false
turn to movement direction = true
'@
    [IO.File]::WriteAllText($baselineSettings,$settingsText,[Text.UTF8Encoding]::new($false))
    $baselineIdentity=Get-OptimizedMWFrozenFile $baselineSettings
    [IO.File]::Copy($baselineSettings,$privateSettings,$false)
    Set-OptimizedMWRouteIniValue -Path $privateSettings -Section 'Game' -Key 'player movement ignores animation' -Value 'true'
    $settingSection='';$gameMovementValues=[Collections.Generic.List[string]]::new();$cameraMovementValues=[Collections.Generic.List[string]]::new()
    foreach($line in [IO.File]::ReadAllLines($privateSettings)){
        if($line -match '^\s*\[([^\]]+)\]\s*$'){$settingSection=$matches[1]}
        if($line -match '^\s*player movement ignores animation\s*=\s*(.*)$'){
            if($settingSection -ieq 'Game'){$gameMovementValues.Add($matches[1])}
            if($settingSection -ieq 'Camera'){$cameraMovementValues.Add($matches[1])}
        }
    }
    Check-Playback ($gameMovementValues.Count -eq 1 -and $gameMovementValues[0] -ceq 'true') 'Private movement override retained duplicate/conflicting Game settings'
    Check-Playback ($cameraMovementValues.Count -eq 1 -and $cameraMovementValues[0] -ceq 'false') 'Private Game override modified another section'
    $derivedText=[IO.File]::ReadAllText($privateSettings)
    Check-Playback ($derivedText.Contains('# keep this comment') -and $derivedText.Contains('smooth movement = true') -and $derivedText.Contains('turn to movement direction = true') -and $derivedText.Contains('fullscreen = false')) 'Private movement override modified unrelated configuration'
    $baselineAfter=Get-OptimizedMWFrozenFile $baselineSettings
    Check-Playback ($baselineAfter.sha256 -ceq $baselineIdentity.sha256 -and $baselineAfter.bytes -eq $baselineIdentity.bytes -and [IO.File]::ReadAllText($baselineSettings) -ceq $settingsText) 'Private override modified original frozen configuration'
    $derivedIdentity=Get-OptimizedMWFrozenFile $privateSettings
    Set-OptimizedMWRouteIniValue -Path $privateSettings -Section 'Game' -Key 'player movement ignores animation' -Value 'true'
    $derivedAfter=Get-OptimizedMWFrozenFile $privateSettings
    Check-Playback ($derivedAfter.sha256 -ceq $derivedIdentity.sha256 -and $derivedAfter.bytes -eq $derivedIdentity.bytes) 'Repeated private movement override is not idempotent'
    $missingSection=Join-Path $fixture 'private-settings-no-game.cfg'
    [IO.File]::WriteAllText($missingSection,"[Video]`nfullscreen = false`n",[Text.UTF8Encoding]::new($false))
    Set-OptimizedMWRouteIniValue -Path $missingSection -Section 'Game' -Key 'player movement ignores animation' -Value 'true'
    Check-Playback ([IO.File]::ReadAllText($missingSection).Contains('[Game]') -and [IO.File]::ReadAllText($missingSection).Contains('player movement ignores animation = true')) 'Private movement override failed to append an absent Game section'

    $valid=Export-PlaybackFixture (New-PlaybackFixtureRows)
    Check-Playback ($valid.playback_complete -and $valid.input_replay_complete -and $valid.playback_protocol -ceq 'raw_physics_input' -and $valid.errors.Count -eq 0 -and $valid.record_count -eq 5) 'Valid legacy physical input preview was rejected'
    Check-Playback ($valid.qualification -ceq 'PREVIEW_NOT_BENCHMARK_VALIDATED' -and $valid.finish.benchmark_qualified -eq $false) 'Preview falsely claims benchmark qualification'
    Check-Playback ($valid.events.check -eq 2 -and (Get-OptimizedMWFrozenFile $valid.playback_file.path).sha256 -ceq $valid.playback_file.sha256) 'Exported observation count/identity unavailable'

    # Guided completion is explicitly distinct from exact raw input completion.
    $guided=Export-PlaybackFixture (New-GuidedPlaybackFixtureRows)
    Check-Playback ($guided.playback_complete -and -not $guided.input_replay_complete -and $guided.playback_protocol -ceq 'physical_xy_follower_v1' -and $guided.errors.Count -eq 0) 'Valid guided preview was rejected or falsely claims raw replay'
    Check-Playback ($guided.finish.route_playback_finished -eq $true -and $guided.finish.input_replay_finished -eq $false -and $guided.qualification -ceq 'PREVIEW_NOT_BENCHMARK_VALIDATED') 'Guided completion/provenance lost during export'
    $rows=New-PlaybackFixtureRows;$rows[4].playback_protocol='raw_physics_input';$rows[4].route_playback_finished=$true
    $explicitRaw=Export-PlaybackFixture $rows
    Check-Playback ($explicitRaw.playback_complete -and $explicitRaw.input_replay_complete -and $explicitRaw.playback_protocol -ceq 'raw_physics_input') 'Explicit raw protocol rejected'
    $rows=New-GuidedPlaybackFixtureRows;$rows[4].input_replay_finished=$true;$null=Check-IncompletePlayback $rows 'Guided preview falsely claiming exact raw replay accepted'
    $rows=New-GuidedPlaybackFixtureRows;$rows[4].input_replay_finished='false';$null=Check-IncompletePlayback $rows 'String raw completion flag accepted for guided preview'
    $rows=New-GuidedPlaybackFixtureRows;$rows[4].input_replay_finished=$null;$null=Check-IncompletePlayback $rows 'Unavailable raw completion flag accepted for guided preview'
    $rows=New-GuidedPlaybackFixtureRows;$rows[4].route_playback_finished=$false;$null=Check-IncompletePlayback $rows 'Unfinished guided route accepted'
    $rows=New-GuidedPlaybackFixtureRows;$rows[4].route_playback_finished='true';$null=Check-IncompletePlayback $rows 'String route completion flag accepted'
    $rows=New-GuidedPlaybackFixtureRows;$rows[4].Remove('route_playback_finished');$null=Check-IncompletePlayback $rows 'Guided footer without route completion accepted'
    $rows=New-GuidedPlaybackFixtureRows;$rows[4].playback_protocol='physical_xy_follower_v99';$null=Check-IncompletePlayback $rows 'Unsupported guided protocol accepted'
    $rows=New-GuidedPlaybackFixtureRows;$rows[4].playback_protocol='Physical_xy_follower_v1';$null=Check-IncompletePlayback $rows 'Case-mutated guided protocol accepted'
    $rows=New-PlaybackFixtureRows;$rows[4].route_playback_finished=$false;$null=Check-IncompletePlayback $rows 'Legacy true flag overrides explicit unfinished route'
    $rows=New-PlaybackFixtureRows;$rows[4].playback_protocol='raw_physics_input';$rows[4].route_playback_finished=$true;$rows[4].input_replay_finished=$false;$null=Check-IncompletePlayback $rows 'Raw protocol with unfinished input replay falsely claims exact raw completion'
    $rows=New-GuidedPlaybackFixtureRows;$null=Check-IncompletePlayback $rows 'Guided preview with nonzero native exit accepted' -ExitCode 17
    $rows=New-GuidedPlaybackFixtureRows;$rows[4].policy_observer_drained=$false;$null=Check-IncompletePlayback $rows 'Guided preview with undrained observer accepted'
    $rows=New-GuidedPlaybackFixtureRows;$null=Check-IncompletePlayback $rows[0..3] 'Guided preview without footer accepted'

    # Physical collision provenance must stay affirmative throughout guided work.
    foreach($rowIndex in @(0,1,2,4)){
        foreach($invalidCollision in @($false,'true',1)){
            $rows=New-GuidedPlaybackFixtureRows
            $rows[$rowIndex].physical_state.collision_enabled=$invalidCollision
            $null=Check-IncompletePlayback $rows ('Guided '+$rows[$rowIndex].event+' accepted invalid collision state '+[string]$invalidCollision)
        }
        $rows=New-GuidedPlaybackFixtureRows;$rows[$rowIndex].physical_state.Remove('collision_enabled')
        $null=Check-IncompletePlayback $rows ('Guided '+$rows[$rowIndex].event+' accepted missing collision flag')
        $rows=New-GuidedPlaybackFixtureRows;$rows[$rowIndex].Remove('physical_state')
        $null=Check-IncompletePlayback $rows ('Guided '+$rows[$rowIndex].event+' accepted missing physical state')
    }

    # Pre-route alignment and explicit source-clock holds are preview observations.
    $rows=New-GuidedPlaybackFixtureRows
    $alignmentCheck=[ordered]@{event='alignment_check';actual_position=@{x=1;y=2;z=3};expected_position=@{x=1;y=2;z=3};command=@{movement=0;side_movement=0};physical_state=$rows[0].physical_state;elapsed=0.2}
    $alignmentFinished=[ordered]@{event='alignment_finished';initial_seed_position=@{x=1;y=2;z=3};actual_position=@{x=1;y=2;z=3};expected_position=@{x=1;y=2;z=3};horizontal_error=0;vertical_error=0;alignment_seconds=0.2;physical_state=$rows[0].physical_state}
    $hold=[ordered]@{event='reference_hold';reference_time=0.5;source_elapsed=0.5;reference_clock_lag=0;horizontal_error=100;vertical_error=0}
    $resume=[ordered]@{event='reference_resume';reference_time=0.5;source_elapsed=0.7;reference_clock_lag=0.2;horizontal_error=10;vertical_error=0}
    $transitions=Export-PlaybackFixture @($rows[0],$alignmentCheck,$alignmentFinished,$rows[1],$rows[2],$hold,$resume,$rows[3],$rows[4])
    Check-Playback ($transitions.playback_complete -and -not $transitions.input_replay_complete -and $transitions.record_count -eq 9 -and $transitions.errors.Count -eq 0) 'Valid guided alignment/hold transitions were rejected or falsely claim raw completion'
    Check-Playback ($transitions.events.alignment_check -eq 1 -and $transitions.events.alignment_finished -eq 1 -and $transitions.events.reference_hold -eq 1 -and $transitions.events.reference_resume -eq 1 -and $transitions.events.check -eq 2) 'Guided transition observations lost in export'
    $rows=New-GuidedPlaybackFixtureRows;$null=Check-IncompletePlayback @($rows[0],$hold,$rows[1],$rows[2],$rows[3],$rows[4]) 'Source clock hold before route start accepted'
    $rows=New-GuidedPlaybackFixtureRows;$null=Check-IncompletePlayback @($rows[0],$resume,$rows[1],$rows[2],$rows[3],$rows[4]) 'Source clock resume before route start accepted'
    $null=Check-IncompletePlayback (New-PlaybackFixtureRows) 'Nonzero native exit was accepted' -ExitCode 17
    $null=Check-IncompletePlayback (New-PlaybackFixtureRows) 'Unavailable native exit was accepted' -ExitCode $null
    $null=Check-IncompletePlayback (New-PlaybackFixtureRows) 'Malformed JSON was accepted' -InjectedRaw '{broken'
    $rows=New-PlaybackFixtureRows;$rows[2].position_error='NaN';$null=Check-IncompletePlayback $rows 'Invalid numeric check was accepted'
    $rows=New-PlaybackFixtureRows;$rows[2].cell_matches='true';$null=Check-IncompletePlayback $rows 'Non-boolean cell check was accepted'
    $rows=New-PlaybackFixtureRows;$rows[2].schema=2;$null=Check-IncompletePlayback $rows 'Mixed schema accepted'
    $rows=New-PlaybackFixtureRows;$rows[2].session=('b'*32);$null=Check-IncompletePlayback $rows 'Mixed session accepted'
    $rows=New-PlaybackFixtureRows;for($i=0;$i -lt $rows.Count;++$i){$rows[$i].seq=$i+1};$rows[2].seq=77;$null=Check-IncompletePlayback $rows 'Discontinuous sequence accepted' -PreserveSequence
    $rows=New-PlaybackFixtureRows;$rows[2].event='unknown';$null=Check-IncompletePlayback $rows 'Unknown observer event was accepted'
    $rows=New-PlaybackFixtureRows;$null=Check-IncompletePlayback $rows[0..3] 'Missing final observer footer accepted'
    $rows=New-PlaybackFixtureRows;$rows[4].policy_stop_acknowledged=$false;$null=Check-IncompletePlayback $rows 'Missing final observer acknowledgment accepted'
    $rows=New-PlaybackFixtureRows;$rows[4].policy.qualified=$false;$null=Check-IncompletePlayback $rows 'Unqualified final policy accepted'
    $rows=New-PlaybackFixtureRows;$rows[4].policy.failed=$true;$null=Check-IncompletePlayback $rows 'Failed final policy accepted'
    $rows=New-PlaybackFixtureRows;$rows[4].policy.combat_during_recording=1;$null=Check-IncompletePlayback $rows 'Final combat accepted'
    $rows=New-PlaybackFixtureRows;$rows[4].policy.pending_disable=1;$null=Check-IncompletePlayback $rows 'Pending final suppression accepted'
    $rows=New-PlaybackFixtureRows;$rows[4].policy.failure_count=1;$null=Check-IncompletePlayback $rows 'Failed suppression accepted'
    $rows=New-PlaybackFixtureRows;$rows[4].benchmark_qualified=$true;$null=Check-IncompletePlayback $rows 'Benchmark claim in preview accepted'
    $rows=New-PlaybackFixtureRows;$rows[4].reason='user_aborted';$null=Check-IncompletePlayback $rows 'Aborted playback accepted'
    $rows=New-PlaybackFixtureRows;$rows[4].input_replay_finished=$false;$null=Check-IncompletePlayback $rows 'Unfinished input replay accepted'
    $rows=New-PlaybackFixtureRows;$rows[4].check_count=3;$null=Check-IncompletePlayback $rows 'Footer check count corruption accepted'
    $rows=New-PlaybackFixtureRows;$null=Check-IncompletePlayback (@($rows[0],$rows[1],$rows[2],$rows[4])) 'Insufficient path observations accepted'
    $rows=New-PlaybackFixtureRows;$rows[3].event='combat';$null=Check-IncompletePlayback $rows 'Observed combat event accepted'
    $rows=New-PlaybackFixtureRows;$rows[3].event='error';$null=Check-IncompletePlayback $rows 'Observer failure event accepted'

    # Footer order and observer drain are independent from numerical path agreement.
    $rows=New-PlaybackFixtureRows;$null=Check-IncompletePlayback (@($rows[4],$rows[0],$rows[1],$rows[2],$rows[3])) 'Finish before setup/start/checks accepted'
    $rows=New-PlaybackFixtureRows;$extra=New-PlaybackFixtureRows;$null=Check-IncompletePlayback (@($rows)+@($extra[2])) 'Observation after final footer accepted'
    $rows=New-PlaybackFixtureRows;$rows[4].policy.ready=$false;$null=Check-IncompletePlayback $rows 'Final policy not ready accepted'
    $rows=New-PlaybackFixtureRows;$rows[4].policy.enabled=$false;$null=Check-IncompletePlayback $rows 'Disabled final policy accepted'
    $rows=New-PlaybackFixtureRows;$rows[4].policy.combat_before_start=1;$null=Check-IncompletePlayback $rows 'Combat before replay accepted'
    $rows=New-PlaybackFixtureRows;$rows[4].policy_observer_drained=$false;$null=Check-IncompletePlayback $rows 'Undrained final observer accepted'
    $rows=New-PlaybackFixtureRows;$rows[4].policy_drain_timed_out=$true;$null=Check-IncompletePlayback $rows 'Final observer timeout accepted'

    $preserved=Join-Path $fixture 'existing.jsonl';[IO.File]::WriteAllText($preserved,'preserve me')
    Reject-Playback {Export-OptimizedMWPlaybackLog -LogPath $valid.playback_file.path -OutputPath $preserved -SessionId $session -ExitCode 0} 'Existing output was overwritten'
    Check-Playback ([IO.File]::ReadAllText($preserved) -ceq 'preserve me') 'Existing evidence was modified'
    Reject-Playback {Export-OptimizedMWPlaybackLog -LogPath $valid.playback_file.path -OutputPath (Join-Path $fixture 'bad-id.jsonl') -SessionId 'not-a-session' -ExitCode 0} 'Invalid session ID accepted'
    $identityPath=Join-Path $fixture 'prepared.lua';[IO.File]::WriteAllText($identityPath,'return {}')
    $identity=Get-OptimizedMWFrozenFile $identityPath
    Assert-OptimizedMWPlaybackIdentities @($identity);Check-Playback $true 'Prepared input identity unexpectedly failed'
    [IO.File]::AppendAllText($identityPath,' -- changed')
    Reject-Playback {Assert-OptimizedMWPlaybackIdentities @($identity)} 'Changed prepared input identity accepted'
    Write-Host ('Playback host checks passed: '+$checks)
}finally{
    # Delete only this exact, verified fixture beneath the system temporary root.
    $tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/')+'\'
    $resolved=[IO.Path]::GetFullPath($fixture)
    if(-not $resolved.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($resolved) -notlike 'OptimizedMW Playback Host *'){throw 'Unsafe fixture cleanup target'}
    if([IO.Directory]::Exists($resolved)){Remove-Item -LiteralPath $resolved -Recurse -Force}
}
