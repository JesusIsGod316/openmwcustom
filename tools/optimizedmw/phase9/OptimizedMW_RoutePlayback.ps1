param(
    [string]$GameDir=$PSScriptRoot,
    [string]$CaptureDirectory='',
    [string]$FrozenConfigDirectory='',
    [string]$PythonExecutable='',
    [switch]$PrepareOnly,
    [switch]$LibraryOnly
)
$ErrorActionPreference='Stop'
# Dot-sourced recorder parameters must not replace this invocation's switches.
$playbackInvocation=@{Game=$GameDir;Capture=$CaptureDirectory;Frozen=$FrozenConfigDirectory;Python=$PythonExecutable;Prepare=[bool]$PrepareOnly;Library=[bool]$LibraryOnly}
. (Join-Path $PSScriptRoot 'OptimizedMW_FrozenConfig.ps1')
. (Join-Path $PSScriptRoot 'OptimizedMW_RouteRecorder.ps1') -GameDir $playbackInvocation.Game -LibraryOnly

function Assert-OptimizedMWPlaybackIdentities {
    param([object[]]$Identities)
    foreach($identity in $Identities){
        $actual=Get-OptimizedMWFrozenFile $identity.path
        if($actual.sha256 -cne $identity.sha256 -or $actual.bytes -ne $identity.bytes){throw ('Playback input changed: '+$identity.path)}
    }
}

function New-OptimizedMWPlaybackSession {
    param([string]$GameDirectory,[string]$Capture,[string]$Frozen,[string]$Python)
    $game=Assert-OptimizedMWPlainPath $GameDirectory
    $capture=Assert-OptimizedMWPlainPath $Capture
    if(-not $Frozen){$Frozen=Join-Path $game 'Configuration\Benchmark-SaintJs-Balmora'}
    if(-not $Python){$Python=(Get-Command python.exe -ErrorAction Stop).Source}
    $pythonIdentity=Get-OptimizedMWFrozenFile $Python
    $package=Assert-OptimizedMWPlainPath (Join-Path $game 'RoutePlayback')
    $session=New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $Frozen -PackageDirectory (Join-Path $game 'RouteRecorder') -RouteName balmora -SmallValidationWindow -OutputCategory Playback
    $data=Join-Path $session.runtime.user_data_directory 'data'
    $relativeFiles=@('optimizedmw_route_playback.omwscripts','scripts\optimizedmw_route_playback\core.lua','scripts\optimizedmw_route_playback\player.lua')
    $playbackInputs=[Collections.Generic.List[object]]::new()
    foreach($relative in $relativeFiles){
        $source=Get-OptimizedMWFrozenFile (Join-Path $package $relative)
        $target=Assert-OptimizedMWPlainPath (Join-Path $data $relative)
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
        [IO.File]::Copy($source.path,$target,$false)
        $copied=Get-OptimizedMWFrozenFile $target
        if($source.sha256 -cne $copied.sha256 -or $source.bytes -ne $copied.bytes){throw 'Playback package copy mismatch'}
        $playbackInputs.Add($source);$playbackInputs.Add($copied)
    }
    $compiler=Get-OptimizedMWFrozenFile (Join-Path $package 'compile_route.py')
    $timeline=Join-Path $data 'scripts\optimizedmw_route_playback\timeline.lua'
    $receiptPath=Join-Path $session.directory 'COMPILED.json'
    $compilerOutput=@(& $Python $compiler.path --capture $capture --output $timeline --receipt $receiptPath)
    if($LASTEXITCODE -ne 0){throw 'Playback compiler refused the capture'}
    Write-Host ($compilerOutput -join "`n")
    $receipt=[IO.File]::ReadAllText($receiptPath) | ConvertFrom-Json
    if($receipt.qualification -cne 'COMPILED_NOT_REPLAY_VALIDATED' -or -not $receipt.source_evidence_unchanged){throw 'Playback compiler receipt is unqualified'}
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
    [IO.File]::WriteAllText($options,("return { enabled=true, session='"+$session.id+"', autostart_seconds=3, quit_on_finish=true, setup_timeout_seconds=90, position_tolerance=128, abort_position_error=2048, max_duration_seconds=180 }"),[Text.UTF8Encoding]::new($false))
    [IO.File]::AppendAllText($session.runtime.config_path,"`ncontent=optimizedmw_route_playback.omwscripts`n",[Text.UTF8Encoding]::new($false))
    $session.arguments=@(Get-OptimizedMWFrozenArguments $session.runtime)+@('--load-savegame',('"'+$privateStart+'"'),'--skip-menu')
    $session.manifest.arguments=$session.arguments
    $session.manifest.options=Get-OptimizedMWFrozenFile $recorderOptions
    $session.manifest.derived_private_inputs=@((Get-OptimizedMWFrozenFile $session.runtime.config_path),(Get-OptimizedMWFrozenFile $session.runtime.settings_path),(Get-OptimizedMWFrozenFile (Join-Path $session.runtime.directory 'input_v3.xml')))
    $session.manifest.mode='private_physics_input_replay_preview'
    $session.manifest.scope='Visible private input playback with path/camera observations. No optimization benchmark or performance acceptance.'
    $session.manifest.starting_location_mode='load_copied_captured_start_no_teleport'
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
    $failures=[Collections.Generic.List[string]]::new();$counts=@{};$seq=0L;$finish=$null;$started=$false;$closed=$false
    $checks=[Collections.Generic.List[object]]::new()
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
            if($closed){$failures.Add('Observation follows playback footer')}
            if($seq -eq 1 -and $event -cne 'setup'){$failures.Add('Playback setup is not the first observation')}
            if($event -eq 'start'){$started=$true}
            if($event -in @('check','marker','finish') -and -not $started){$failures.Add('Playback work/footer precedes start')}
            if($event -notin @('setup','start','check','marker','finish','error','combat')){$failures.Add('Unknown playback event')}
            if(-not $counts.ContainsKey($event)){$counts[$event]=0};$counts[$event]++
            if($event -eq 'check'){
                if(-not (Test-OptimizedMWRouteNumber $record.position_error 0 1e8) -or $record.cell_matches -isnot [bool]){$failures.Add('Invalid playback position/cell check')}
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
        if([string]$finish.reason -cne 'route_finished' -or $finish.input_replay_finished -isnot [bool] -or -not $finish.input_replay_finished){$failures.Add('Input replay did not finish its route')}
        if($finish.policy_stop_acknowledged -isnot [bool] -or -not $finish.policy_stop_acknowledged -or
            $finish.policy.qualified -isnot [bool] -or -not $finish.policy.qualified -or $finish.policy.failed -isnot [bool] -or $finish.policy.failed -or
            $finish.policy.ready -isnot [bool] -or -not $finish.policy.ready -or $finish.policy.enabled -isnot [bool] -or -not $finish.policy.enabled -or
            $finish.policy.combat_before_start -ne 0 -or $finish.policy.combat_during_recording -ne 0 -or $finish.policy.pending_disable -ne 0 -or $finish.policy.failure_count -ne 0 -or
            $finish.policy_observer_drained -isnot [bool] -or -not $finish.policy_observer_drained -or $finish.policy_drain_timed_out -isnot [bool] -or $finish.policy_drain_timed_out){$failures.Add('Final actor policy unavailable or unqualified')}
        if($finish.benchmark_qualified -isnot [bool] -or $finish.benchmark_qualified){$failures.Add('Preview incorrectly claims benchmark qualification')}
        if($finish.check_count -ne $checks.Count){$failures.Add('Playback check-count mismatch')}
    }
    return [pscustomobject][ordered]@{schema=1;session=$SessionId;input_replay_complete=($failures.Count -eq 0);qualification='PREVIEW_NOT_BENCHMARK_VALIDATED';
        exit_code=$ExitCode;errors=@($failures | Select-Object -Unique);record_count=$seq;events=$counts;finish=$finish;
        playback_file=(Get-OptimizedMWFrozenFile $OutputPath);scope='Physical input replay preview; path errors and cell mismatches require review before automated benchmarking.'}
}

function Invoke-OptimizedMWRoutePlayback {
    $game=Assert-OptimizedMWPlainPath $playbackInvocation.Game
    if(@(Get-Process -Name openmw -ErrorAction SilentlyContinue).Count){throw 'Close the existing OpenMW instance before playback'}
    $capture=$playbackInvocation.Capture
    if(-not $capture){
        $candidates=@(Get-ChildItem -LiteralPath (Join-Path $game 'Benchmarks\Routes') -Directory | Where-Object {$_.Name -like 'balmora_*'} | Sort-Object LastWriteTimeUtc -Descending)
        foreach($candidate in $candidates){
            $summary=Join-Path $candidate.FullName 'SUMMARY.json'
            if(-not [IO.File]::Exists($summary)){continue}
            $status=[IO.File]::ReadAllText($summary) | ConvertFrom-Json
            if($status.complete -eq $true -and $status.qualification -ceq 'RECORDED_NOT_REPLAY_VALIDATED'){$capture=$candidate.FullName;break}
        }
        if(-not $capture){throw 'No complete Balmora recording found'}
    }
    $session=New-OptimizedMWPlaybackSession -GameDirectory $game -Capture $capture -Frozen $playbackInvocation.Frozen -Python $playbackInvocation.Python
    Write-Host ('Source recording: '+$capture)
    Write-Host ('Playback evidence: '+$session.directory)
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
        try{Assert-OptimizedMWPlaybackIdentities $session.manifest.source_capture}catch{$sourceUnchanged=$false;$summary.errors+=@($_.Exception.Message);$summary.input_replay_complete=$false}
        try{
            $protectedAfter=Get-OptimizedMWRouteProtectedInputs $session.baseline
            $unchanged=Compare-OptimizedMWRouteProtectedInputs $session.manifest.protected_before $protectedAfter
        }catch{$unchanged=$false;$summary.errors+=@($_.Exception.Message)}
        if(-not $unchanged){$summary.errors+=@('Protected original inputs changed during playback');$summary.input_replay_complete=$false}
        $summary | Add-Member -NotePropertyName protected_inputs_unchanged -NotePropertyValue $unchanged
        $summary | Add-Member -NotePropertyName source_capture_unchanged -NotePropertyValue $sourceUnchanged
        Write-OptimizedMWRouteJson (Join-Path $session.directory 'SUMMARY.json') $summary
        if(-not $summary.input_replay_complete){Write-Warning ('Playback incomplete: '+($summary.errors -join '; '))}
        return $summary
    }finally{foreach($name in $environment.Keys){[Environment]::SetEnvironmentVariable($name,$environment[$name],'Process')}}
}
if(-not $playbackInvocation.Library){Invoke-OptimizedMWRoutePlayback}
