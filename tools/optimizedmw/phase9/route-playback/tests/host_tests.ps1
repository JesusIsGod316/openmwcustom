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
    Check-Playback (-not $result.input_replay_complete -and $result.errors.Count -gt 0) $Message
    return $result
}
try{
    # Actual dot-source parameter binding must retain playback invocation values.
    . $hostPath -GameDir $fixture -CaptureDirectory (Join-Path $fixture 'capture') -FrozenConfigDirectory (Join-Path $fixture 'frozen') -PythonExecutable (Join-Path $fixture 'python.exe') -PrepareOnly -LibraryOnly
    Check-Playback ($playbackInvocation.Prepare -eq $true -and $playbackInvocation.Library -eq $true) 'Dot-sourced recorder replaced playback switches'
    Check-Playback ($playbackInvocation.Game -ceq $fixture -and $playbackInvocation.Capture -ceq (Join-Path $fixture 'capture') -and $playbackInvocation.Frozen -ceq (Join-Path $fixture 'frozen') -and $playbackInvocation.Python -ceq (Join-Path $fixture 'python.exe')) 'Dot-source replaced selected playback inputs'
    $tokens=$null;$parseErrors=$null;$null=[Management.Automation.Language.Parser]::ParseFile($hostPath,[ref]$tokens,[ref]$parseErrors)
    Check-Playback ($parseErrors.Count -eq 0) 'Playback host source does not parse'

    $valid=Export-PlaybackFixture (New-PlaybackFixtureRows)
    Check-Playback ($valid.input_replay_complete -and $valid.errors.Count -eq 0 -and $valid.record_count -eq 5) 'Valid physical input preview was rejected'
    Check-Playback ($valid.qualification -ceq 'PREVIEW_NOT_BENCHMARK_VALIDATED' -and $valid.finish.benchmark_qualified -eq $false) 'Preview falsely claims benchmark qualification'
    Check-Playback ($valid.events.check -eq 2 -and (Get-OptimizedMWFrozenFile $valid.playback_file.path).sha256 -ceq $valid.playback_file.sha256) 'Exported observation count/identity unavailable'
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
