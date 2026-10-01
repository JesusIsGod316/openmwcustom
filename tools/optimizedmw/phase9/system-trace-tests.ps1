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

# Exact path filtering must reject another installation even with the same name,
# while birth time disambiguates a recycled PID.
function Get-CimInstance {
    param([string]$ClassName,[string]$Filter)
    Assert-Trace ($ClassName -eq 'Win32_Process' -and $Filter -eq "Name='openmw.exe'") 'Unexpected process query'
    [pscustomobject]@{ProcessId=77;CreationDate=[DateTime]::Parse('2026-10-01T10:00:00Z');ExecutablePath='C:\test game\openmw.exe'}
    [pscustomobject]@{ProcessId=88;CreationDate=[DateTime]::Parse('2026-10-01T10:00:01Z');ExecutablePath='C:\other game\openmw.exe'}
    [pscustomobject]@{ProcessId=99;CreationDate=[DateTime]::Parse('2026-10-01T10:00:02Z');ExecutablePath=$null}
}
function Get-Process {
    param([int]$Id,[string]$ErrorAction)
    Assert-Trace ($Id -eq 77) 'Attempted to identify a different executable'
    [pscustomobject]@{StartTime=[DateTime]::Parse('2026-10-01T10:00:00Z').AddTicks(7);Path='C:\test game\openmw.exe'}
}
$games=@(Get-SystemTraceGameProcesses 'C:\test game\openmw.exe')
Assert-Trace ($games.Count -eq 1 -and $games[0].process_id -eq 77 -and $games[0].creation_utc -eq '2026-10-01T10:00:00.0000007Z') 'Path/birth identity or native timestamp precision was lost'

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
    $archiveState=@{schema=2;normal_stop=$true;status='saved';etl_sha256=(Get-FileHash -LiteralPath $raw).Hash}
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
Write-Host 'PASS system trace: File/Memory controls, duration/space/exit guards, path/PID lifetime, owned-instance failure cleanup, retention/loss/identity/GPU qualification, atomic sidecars and streaming-verified archive with raw preservation'
