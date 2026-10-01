param([string]$ExistingCaptureDirectory)
$ErrorActionPreference='Stop'
foreach($name in @('OptimizedMW_SystemTrace.ps1','OptimizedMW_QualifySystemTrace.ps1','system-trace-tests.ps1')){
    $tokens=$null;$errors=$null
    $null=[System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $name),[ref]$tokens,[ref]$errors)
    if($errors.Count){throw ($errors | Out-String)}
}
. (Join-Path $PSScriptRoot 'OptimizedMW_SystemTrace.ps1') -FunctionsOnly
. (Join-Path $PSScriptRoot 'OptimizedMW_QualifySystemTrace.ps1') -CaptureDirectory 'unused' -FunctionsOnly
function Assert-Trace([bool]$Condition,[string]$Message){if(-not $Condition){throw $Message}}

$file=@(Get-SystemTraceStartArguments 'C:\trace profile.wprp' 'own-instance' 'File' 'C:\owned temp')
$memory=@(Get-SystemTraceStartArguments 'C:\trace profile.wprp' 'own-instance' 'Memory' 'C:\owned temp')
Assert-Trace ($file -contains '-filemode' -and $file -contains '-recordtempto' -and $file[-1] -eq 'C:\owned temp') 'Full run does not use the owned sequential file target'
Assert-Trace ($memory -notcontains '-filemode' -and $memory -notcontains '-recordtempto') 'Circular control was lost'
Assert-Trace ($null -eq (Get-SystemTraceStopReason 30 360 11GB 10GB $false $false 0)) 'Recorder stopped before the game started'
Assert-Trace ((Get-SystemTraceStopReason 30 360 11GB 10GB $false $true 0) -eq 'game_exit') 'Game exit not detected'
Assert-Trace ($null -eq (Get-SystemTraceStopReason 30 360 11GB 10GB $false $true 1)) 'Running game prematurely stopped recorder'
Assert-Trace ((Get-SystemTraceStopReason 360 360 11GB 10GB $false $true 1) -eq 'duration_limit') 'Duration bound was not enforced'
Assert-Trace ((Get-SystemTraceStopReason 20 360 9GB 10GB $false $true 1) -eq 'minimum_free_space') 'Low-space recording continued'
Assert-Trace ((Get-SystemTraceStopReason 20 360 11GB 10GB $true $false 0) -eq 'user_enter') 'Manual early stop was not honored'

# A normal Windows engine spawns the same executable with the raw command line
# `--crash-monitor <hex SHM handle>`; only that grammar identifies the helper.
$testExecutable='C:\test game\openmw.exe'
foreach($command in @('--crash-monitor 00000000000000DC','--crash-monitor 0xdc',
    ' --crash-monitor ABC123 ',('"'+$testExecutable+'" --crash-monitor DC'),
    ($testExecutable+' --crash-monitor DC'))){
    Assert-Trace ((Get-SystemTraceProcessRole $command $testExecutable) -eq 'crash_monitor') ('Real crash helper was counted as a game: '+$command)
}
foreach($command in @($null,'','--crash-monitor','--crash-monitor 0','--crash-monitor 0x0000',
    '--crash-monitor xyz','--crash-monitor 12345678901234567','--crash-monitor DC extra',
    '--other=--crash-monitor DC','--CRASH-MONITOR DC','--crash-monitor 0XDC',
    ('--crash-monitor'+[char]0x00a0+'DC'),
    '"C:\other game\openmw.exe" --crash-monitor DC')){
    Assert-Trace ((Get-SystemTraceProcessRole $command $testExecutable) -eq 'unknown') ('Unconfirmed process was hidden as a helper: '+$command)
}
Assert-Trace ((Get-SystemTraceProcessRole ('"'+$testExecutable+'" --config="C:\test profile"') $testExecutable) -eq 'engine_candidate') 'Normal engine command line was not a candidate'

# Exact path filtering rejects another installation even with the same name.
# Native birth retains 100 ns precision; CIM's microsecond identity corroborates
# command line and parent metadata, rejecting a recycled PID between lookups.
$birth=[DateTime]::Parse('2026-10-01T10:00:00Z')
$script:mockProcesses=@(
    [pscustomobject]@{ProcessId=77;ParentProcessId=10;CreationDate=$birth;ExecutablePath=$testExecutable;CommandLine=('"'+$testExecutable+'" --config="C:\profile"')},
    [pscustomobject]@{ProcessId=78;ParentProcessId=77;CreationDate=$birth.AddSeconds(1);ExecutablePath=$testExecutable;CommandLine='--crash-monitor 00000000000000DC'},
    [pscustomobject]@{ProcessId=88;ParentProcessId=10;CreationDate=$birth.AddSeconds(2);ExecutablePath='C:\other game\openmw.exe';CommandLine='openmw.exe'},
    [pscustomobject]@{ProcessId=99;ParentProcessId=10;CreationDate=$birth.AddSeconds(3);ExecutablePath=$null;CommandLine=$null})
$script:mockNativeProcesses=@{
    77=[pscustomobject]@{StartTime=$birth.AddTicks(7);Path=$testExecutable};
    78=[pscustomobject]@{StartTime=$birth.AddSeconds(1).AddTicks(7);Path=$testExecutable}}
function Get-CimInstance {
    param([string]$ClassName,[string]$Filter)
    Assert-Trace ($ClassName -eq 'Win32_Process' -and $Filter -eq "Name='openmw.exe'") 'Unexpected process query'
    $script:mockProcesses
}
$script:processRace='none';$script:racePropertyReads=0
function Get-Process {
    param([int]$Id,[string]$ErrorAction)
    Assert-Trace ($script:mockNativeProcesses.ContainsKey($Id)) 'Attempted to identify a different executable'
    if($Id -ne 77){return $script:mockNativeProcesses[$Id]}
    if($script:processRace -eq 'absent'){return $null}
    if($script:processRace -eq 'lookup'){throw 'Process exited during native lookup'}
    if($script:processRace -eq 'path'){
        $result=[pscustomobject]@{StartTime=[DateTime]::Parse('2026-10-01T10:00:00Z')}
        Add-Member -InputObject $result -MemberType ScriptProperty -Name Path -Value {
            $script:racePropertyReads++;throw 'Process exited before Path was read'
        }
        return $result
    }
    if($script:processRace -eq 'birth'){
        $result=[pscustomobject]@{Path='C:\test game\openmw.exe'}
        Add-Member -InputObject $result -MemberType ScriptProperty -Name StartTime -Value {
            $script:racePropertyReads++;throw 'Process exited before StartTime was read'
        }
        return $result
    }
    $script:mockNativeProcesses[$Id]
}
$inventory=@(Get-SystemTraceExecutableProcesses $testExecutable)
$games=@(Get-SystemTraceGameProcesses $testExecutable)
Assert-Trace ($games.Count -eq 1 -and $games[0].process_id -eq 77 -and $games[0].creation_utc -eq '2026-10-01T10:00:00.0000007Z') 'Path/birth identity or native timestamp precision was lost'
Assert-Trace ($inventory.Count -eq 2 -and $inventory[1].process_id -eq 78 -and $inventory[1].parent_process_id -eq 77 -and
    $inventory[1].role -eq 'crash_monitor' -and $inventory[1].command_line -eq '--crash-monitor 00000000000000DC') 'Crash monitor inventory/parent/command line was not retained'
$trackedState=@{game_processes=@();helper_processes=@();process_inventory=@();process_inventory_history=@()}
$tracker=[pscustomobject]@{games=@{};helpers=@{};signature=$null}
$snapshot=Update-SystemTraceProcessInventory $trackedState $tracker $inventory
Assert-Trace ($snapshot.games.Count -eq 1 -and $snapshot.helpers.Count -eq 1 -and $snapshot.new_games.Count -eq 1 -and
    -not $snapshot.multiple_games -and $snapshot.game_observed -and $trackedState.helper_processes.Count -eq 1) 'Normal engine/helper pair was misclassified'
$snapshot=Update-SystemTraceProcessInventory $trackedState $tracker $inventory
Assert-Trace (-not $snapshot.changed -and $snapshot.new_games.Count -eq 0 -and $trackedState.process_inventory_history.Count -eq 1) 'Unchanged inventory duplicated history/engine identities'
$helperOnly=@($inventory | Where-Object {$_.role -eq 'crash_monitor'})
$snapshot=Update-SystemTraceProcessInventory $trackedState $tracker $helperOnly
Assert-Trace ($snapshot.games.Count -eq 0 -and $snapshot.helpers.Count -eq 1 -and $snapshot.game_observed -and
    (Get-SystemTraceStopReason 30 360 11GB 10GB $false $snapshot.game_observed $snapshot.games.Count $snapshot.multiple_games) -eq 'game_exit') 'Helper surviving parent exit delayed automatic save'
$snapshot=Update-SystemTraceProcessInventory $trackedState $tracker @()
Assert-Trace ($snapshot.games.Count -eq 0 -and $trackedState.process_inventory_history.Count -eq 3 -and
    $trackedState.game_processes.Count -eq 1 -and $trackedState.helper_processes.Count -eq 1) 'Exited process history was lost'
$emptyState=@{game_processes=@();helper_processes=@();process_inventory=@();process_inventory_history=@()}
$emptyTracker=[pscustomobject]@{games=@{};helpers=@{};signature=$null}
$snapshot=Update-SystemTraceProcessInventory $emptyState $emptyTracker $helperOnly
Assert-Trace (-not $snapshot.game_observed -and
    $null -eq (Get-SystemTraceStopReason 30 360 11GB 10GB $false $snapshot.game_observed $snapshot.games.Count $snapshot.multiple_games)) 'Orphan helper before launch was treated as a game session'

$originalMocks=$script:mockProcesses
$secondGame=[pscustomobject]@{ProcessId=79;ParentProcessId=10;CreationDate=$birth.AddSeconds(2);ExecutablePath=$testExecutable;CommandLine=('"'+$testExecutable+'"')}
$secondHelper=[pscustomobject]@{ProcessId=80;ParentProcessId=79;CreationDate=$birth.AddSeconds(3);ExecutablePath=$testExecutable;CommandLine='--crash-monitor 0xDC'}
$script:mockNativeProcesses[79]=[pscustomobject]@{StartTime=$birth.AddSeconds(2).AddTicks(7);Path=$testExecutable}
$script:mockNativeProcesses[80]=[pscustomobject]@{StartTime=$birth.AddSeconds(3).AddTicks(7);Path=$testExecutable}
$script:mockProcesses=$originalMocks+@($secondGame,$secondHelper)
$inventory=@(Get-SystemTraceExecutableProcesses $testExecutable)
$snapshot=Update-SystemTraceProcessInventory $trackedState $tracker $inventory
Assert-Trace ($snapshot.games.Count -eq 2 -and $snapshot.helpers.Count -eq 2 -and $snapshot.multiple_games -and
    (Get-SystemTraceStopReason 30 360 11GB 10GB $false $snapshot.game_observed $snapshot.games.Count $snapshot.multiple_games) -eq 'multiple_games') 'Two genuine engines were not saved as an ambiguous run'
$script:mockProcesses=$originalMocks

$savedCommand=$script:mockProcesses[0].CommandLine
foreach($command in @($null,'--crash-monitor 0','--crash-monitor DC extra','--other=--crash-monitor DC')){
    $script:mockProcesses[0].CommandLine=$command
    $games=@(Get-SystemTraceGameProcesses $testExecutable)
    Assert-Trace ($games.Count -eq 1 -and $games[0].role -eq 'unknown') 'Unknown command line disappeared from engine candidates'
}
$script:mockProcesses[0].CommandLine=$savedCommand
$savedBirth=$script:mockNativeProcesses[77].StartTime
$script:mockNativeProcesses[77].StartTime=$birth.AddSeconds(10)
Assert-Trace (@(Get-SystemTraceGameProcesses $testExecutable).Count -eq 0) 'Recycled PID inherited prior command line and parent metadata'
$script:mockNativeProcesses[77].StartTime=$savedBirth
foreach($race in @('path','birth','lookup','absent')){
    $script:processRace=$race;$script:racePropertyReads=0
    $games=@(Get-SystemTraceGameProcesses 'C:\test game\openmw.exe')
    Assert-Trace ($games.Count -eq 0) ('Exited process produced a partial identity: '+$race)
    if($race -eq 'path' -or $race -eq 'birth'){
        Assert-Trace ($script:racePropertyReads -eq 1) ('Property disappearance was not exercised: '+$race)
    }
    Assert-Trace ((Get-SystemTraceStopReason 30 360 11GB 10GB $false $true $games.Count) -eq 'game_exit') ('Process disappearance did not preserve automatic save: '+$race)
}
$script:processRace='none'
$games=@(Get-SystemTraceGameProcesses $testExecutable)
Assert-Trace ($games.Count -eq 1) 'Process disappearance poisoned subsequent enumeration'

$script:calls=[Collections.Generic.List[object]]::new();$script:nativeExitCode=0
function Invoke-SystemRecorder {
    param([string[]]$Arguments)
    [void]$script:calls.Add($Arguments)
    [pscustomobject]@{Code=$script:nativeExitCode;Lines=@('mock WPR')}
}
$lease=[pscustomobject]@{instance='owned-123';started=$false}
$script:nativeExitCode=5
$null=Start-SystemTraceOwnedInstance $lease 'profile' 'File' 'temp'
Assert-Trace (-not $lease.started) 'Failed start acquired stop/cancel ownership'
$callsBefore=$script:calls.Count;$null=Cancel-SystemTraceOwnedInstance $lease
Assert-Trace ($script:calls.Count -eq $callsBefore) 'Cleanup cancelled a session after failed start'
$script:nativeExitCode=0;$null=Start-SystemTraceOwnedInstance $lease 'profile' 'File' 'temp'
Assert-Trace $lease.started 'Successful start failed to acquire ownership'
$script:nativeExitCode=5;$null=Save-SystemTraceOwnedInstance $lease 'trace.etl'
Assert-Trace $lease.started 'Failed stop lost cleanup ownership'
$script:nativeExitCode=0;$null=Cancel-SystemTraceOwnedInstance $lease
Assert-Trace (-not $lease.started) 'Successful cleanup retained ownership'
foreach($call in $script:calls){
    $index=[Array]::IndexOf($call,'-instancename')
    Assert-Trace ($index -ge 0 -and $call[$index+1] -eq 'owned-123') 'Operation could affect an unrelated recorder instance'
}
$script:nativeExitCode=0;$null=Start-SystemTraceOwnedInstance $lease 'profile' 'Memory' 'temp'
$null=Save-SystemTraceOwnedInstance $lease 'trace.etl'
$callsBefore=$script:calls.Count;$null=Cancel-SystemTraceOwnedInstance $lease
Assert-Trace ($script:calls.Count -eq $callsBefore) 'Successful save was followed by an unnecessary cancel'

$epoch=[DateTime]::Parse('2026-10-01T10:00:00Z').ToUniversalTime()
$native=[pscustomobject]@{HeaderStartFileTime=$epoch.ToFileTimeUtc();HeaderEndFileTime=$epoch.AddSeconds(180).ToFileTimeUtc();
    FirstSchedulingFileTime=$epoch.ToFileTimeUtc();LastSchedulingFileTime=$epoch.AddSeconds(170).ToFileTimeUtc();
    LostEvents=0;LostBuffers=0;Events=100;SchedulingEvents=20;SampleEvents=10;GpuEvents=0;GpuKinds=@{}}
$state=[pscustomobject]@{capture_id='id';game_processes=@()}
$binding=[pscustomobject]@{identity_valid=$true;start_utc=$epoch.AddSeconds(1);end_utc=$epoch.AddSeconds(160)}
$report=Get-SystemTraceQualification $native $state $binding
Assert-Trace $report.scheduling_complete 'Complete matching engine range rejected'
Assert-Trace ($report.gpu_event_availability.StartsWith('UNAVAILABLE') -and $report.gpu_queue_attribution.StartsWith('UNQUALIFIED')) 'Absent GPU data was claimed as available'
$ambiguousState=[pscustomobject]@{capture_id='id';game_processes=@();ambiguous_game_processes=$true;stop_reason='multiple_games';
    helper_processes=$trackedState.helper_processes;process_inventory=$inventory}
$report=Get-SystemTraceQualification $native $ambiguousState $binding
Assert-Trace (-not $report.scheduling_complete -and $report.ambiguous_game_processes -and $report.stop_reason -eq 'multiple_games') 'Ambiguous engines accepted despite full coverage, zero loss and valid binding'
$native.FirstSchedulingFileTime=$epoch.AddSeconds(150).ToFileTimeUtc()
$report=Get-SystemTraceQualification $native $state $binding
Assert-Trace (-not $report.scheduling_complete -and $report.coverage -eq 'PARTIAL' -and $report.lost_events -eq 0) 'Circular overwrite hidden by zero event loss'
$native.FirstSchedulingFileTime=$epoch.ToFileTimeUtc();$native.LostBuffers=1
Assert-Trace (-not (Get-SystemTraceQualification $native $state $binding).scheduling_complete) 'Lost buffer accepted'
$native.LostBuffers=0;$binding.identity_valid=$false
Assert-Trace (-not (Get-SystemTraceQualification $native $state $binding).scheduling_complete) 'Mismatched PID/run accepted'
$native.GpuEvents=2;$native.GpuKinds=@{'task=1;opcode=2;event=3;version=4'=2}
$report=Get-SystemTraceQualification $native $state $binding
Assert-Trace ($report.dxgkrnl_task_opcode_event_version_counts.Count -eq 1 -and $report.gpu_queue_attribution.StartsWith('UNQUALIFIED')) 'Provider events incorrectly proved GPU queue ownership'

$tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$temp=Join-Path $tempRoot ('p9-system-trace-tests-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try{
    $path=Join-Path $temp 'identity.json'
    Write-SystemTraceJson $path @{capture_id='first'}
    Write-SystemTraceJson $path @{capture_id='second'}
    Assert-Trace ((Get-Content -Raw -LiteralPath $path | ConvertFrom-Json).capture_id -eq 'second') 'Atomic identity replacement failed'
    Assert-Trace (@(Get-ChildItem -LiteralPath $temp -File).Count -eq 1) 'Identity temporary file leaked'
    $state=[pscustomobject]@{schema=2;capture_id='session';instance='owned';recorder_process_id=42;recorder_creation_utc='2026-10-01T10:00:00.0000007Z';normal_stop=$false;status='saving'}
    Write-SystemTraceJson (Join-Path $temp 'SYSTEM-TRACE-STATE.json') $state
    & (Join-Path $PSScriptRoot 'OptimizedMW_QualifySystemTrace.ps1') -CaptureDirectory $temp
    Assert-Trace ((Get-Content -Raw -LiteralPath (Join-Path $temp 'SYSTEM-TRACE-QUALIFICATION-PENDING.json') | ConvertFrom-Json).status -eq 'PENDING_OR_UNAVAILABLE') 'Unsaved ETL did not return explicit pending qualification'
    Assert-Trace (-not (Test-Path -LiteralPath (Join-Path $temp 'SYSTEM-TRACE-QUALIFICATION.json'))) 'Saving recorder incorrectly produced a completeness report'
    $state.normal_stop=$true;$state.status='saved'
    $zipPath=Join-Path $temp 'final.zip';[IO.File]::WriteAllBytes($zipPath,[byte[]]@(1,2,3))
    $final=[pscustomobject]@{capture_id='session';instance='owned';recorder_process_id=42;recorder_creation_utc='2026-10-01T10:00:00.0000007Z';
        engine_zip_path=$zipPath;engine_zip_bytes=3;engine_zip_sha256=(Get-FileHash -LiteralPath $zipPath).Hash}
    # Exercise PS7's automatic JSON date conversion, preserving native 100 ns
    # precision rather than relying on an all-string in-memory fixture.
    $state=$state | ConvertTo-Json | ConvertFrom-Json
    $final=$final | ConvertTo-Json | ConvertFrom-Json
    Assert-Trace ((ConvertTo-SystemTraceUtcText $state.recorder_creation_utc) -eq '2026-10-01T10:00:00.0000007Z') 'Recorder JSON birth precision changed'
    Assert-Trace ((ConvertTo-SystemTraceUtcValue $final.recorder_creation_utc).ToString('o') -eq '2026-10-01T10:00:00.0000007Z') 'Qualifier JSON birth precision changed'
    Assert-Trace ((Resolve-SystemTraceFinalBinding $state $final '').profile_zip -eq $zipPath) 'Final ZIP could not be consumed after saving'
    foreach($field in @('capture_id','instance','recorder_process_id','recorder_creation_utc','engine_zip_sha256','engine_zip_bytes')){
        $saved=$final.$field
        $final.$field=$(if($field -in @('recorder_process_id','engine_zip_bytes')){99}else{'mismatch'})
        $rejected=$false;try{$null=Resolve-SystemTraceFinalBinding $state $final ''}catch{$rejected=$true}
        Assert-Trace $rejected ('Mismatched final binding accepted: '+$field)
        $final.$field=$saved
    }
    [IO.File]::WriteAllBytes($zipPath,[byte[]]@(3,2,1))
    $rejected=$false;try{$null=Resolve-SystemTraceFinalBinding $state $final ''}catch{$rejected=$true}
    Assert-Trace $rejected 'Replaced ZIP with same byte count accepted'
    $source=Join-Path $temp 'system-capture';New-Item -ItemType Directory -Path $source | Out-Null
    $raw=Join-Path $source 'OptimizedMW-SystemTrace.etl';[IO.File]::WriteAllBytes($raw,[byte[]]@(0,1,127,255))
    $archiveState=@{schema=2;normal_stop=$true;status='saved';etl_sha256=(Get-FileHash -LiteralPath $raw).Hash;
        stop_reason='multiple_games';ambiguous_game_processes=$true;process_inventory=$inventory;helper_processes=$trackedState.helper_processes}
    Write-SystemTraceJson (Join-Path $source 'SYSTEM-TRACE-STATE.json') $archiveState
    'profile' | Set-Content -LiteralPath (Join-Path $source 'profile.wprp')
    'incomplete' | Set-Content -LiteralPath (Join-Path $source 'atomic.tmp')
    $wprTemp=Join-Path $source 'WPRTemp';New-Item -ItemType Directory -Path $wprTemp | Out-Null
    'temporary trace' | Set-Content -LiteralPath (Join-Path $wprTemp 'recording.etl')
    $systemZip=Join-Path $temp 'system.zip'
    $verified=New-VerifiedSystemTraceZip $source $systemZip
    Assert-Trace ($verified.files -eq 3 -and $verified.sha256 -eq (Get-FileHash -LiteralPath $systemZip).Hash) 'System archive did not verify its final hash/inventory'
    $readback=[IO.Compression.ZipFile]::OpenRead($systemZip)
    try{
        Assert-Trace ($readback.Entries.Count -eq 3 -and $null -eq $readback.GetEntry('atomic.tmp') -and
            $null -eq $readback.GetEntry('WPRTemp/recording.etl') -and $readback.GetEntry('OptimizedMW-SystemTrace.etl').Length -eq 4) 'Transient recording files leaked into shared archive'
    }finally{$readback.Dispose()}
    $before=(Get-FileHash -LiteralPath $systemZip).Hash
    $rejected=$false;try{$null=New-VerifiedSystemTraceZip $source $systemZip}catch{$rejected=$true}
    Assert-Trace ($rejected -and (Get-FileHash -LiteralPath $systemZip).Hash -eq $before) 'Existing verified system ZIP was replaced'
    $archiveState.etl_sha256='mismatch';Write-SystemTraceJson (Join-Path $source 'SYSTEM-TRACE-STATE.json') $archiveState
    $rawHash=(Get-FileHash -LiteralPath $raw).Hash
    $rejected=$false;try{$null=New-VerifiedSystemTraceZip $source (Join-Path $temp 'bad-system.zip')}catch{$rejected=$true}
    Assert-Trace ($rejected -and -not [IO.File]::Exists((Join-Path $temp 'bad-system.zip')) -and
        (Get-FileHash -LiteralPath $raw).Hash -eq $rawHash -and [IO.File]::Exists((Join-Path $source 'SYSTEM-TRACE-STATE.json'))) 'Failed archive publication changed raw ETL/state or published corrupt ZIP'
}finally{
    $resolved=[IO.Path]::GetFullPath($temp)
    Assert-Trace ($resolved.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase)) 'Unsafe fixture cleanup path'
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
if($ExistingCaptureDirectory){
    # Read-only ETL replay writes only a new qualification report beside the ETL.
    & (Join-Path $PSScriptRoot 'OptimizedMW_QualifySystemTrace.ps1') -CaptureDirectory $ExistingCaptureDirectory
}
Write-Host 'PASS system trace: File/Memory controls, duration/space/exit guards, exact-path engine/crash-helper roles and native PID lifetime, ambiguity preservation, owned-instance failure cleanup, retention/loss/identity/GPU qualification, atomic sidecars and streaming-verified archive with raw preservation'
