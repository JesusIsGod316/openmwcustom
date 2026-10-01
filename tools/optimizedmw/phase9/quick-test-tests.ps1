# Orchestration is exercised through mock elevated helpers. This test never
# starts a game, requests UAC, records ETW or changes the user's OpenMW settings.
param([string]$FixturePrefix='optimizedmw-quick-tests')
$ErrorActionPreference='Stop'
foreach($name in @('OptimizedMW_QuickTest.ps1','OptimizedMW_QuickRecorder.ps1','quick-test-tests.ps1')){
    $tokens=$null;$errors=$null
    $null=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $name),[ref]$tokens,[ref]$errors)
    if($errors.Count){throw ($errors | Out-String)}
}
. (Join-Path $PSScriptRoot 'OptimizedMW_QuickTest.ps1')
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Assert-Quick([bool]$Condition,[string]$Message){if(-not $Condition){throw $Message}}
function Assert-QuickThrows([scriptblock]$Action,[string]$Message){$failed=$false;try{& $Action}catch{$failed=$true};Assert-Quick $failed $Message}
$tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$temp=Join-Path $tempRoot ($FixturePrefix+'-'+[Guid]::NewGuid().ToString('N'))
$game=Join-Path $temp 'nested game [one]\runtime [two]'
$null=New-Item -ItemType Directory -Path $game
[IO.File]::WriteAllBytes((Join-Path $game 'openmw.exe'),[byte[]]@(0,1,2,3,7,255))
$script:administrator=$false;$script:startup='ready';$script:helperExited=$false
$script:helperCalls=0;$script:qualifierCalls=0;$script:qualifierMode='complete';$script:freeBytes=100GB
$ownerBirth=[DateTime]::Parse('2026-10-01T10:00:00.0000007Z').ToUniversalTime()
$helperBirth=$ownerBirth.AddSeconds(1)
function Test-OptimizedMWQuickAdministrator {return $script:administrator}
function Get-OptimizedMWQuickOwner {return [pscustomobject]@{process_id=77;creation_utc='2026-10-01T10:00:00.0000007Z'}}
function Get-Process {
    param([int]$Id,[string]$ErrorAction)
    if($Id -eq 88 -and -not $script:helperExited){return [pscustomobject]@{StartTime=$helperBirth}}
    if($Id -eq 77){return [pscustomobject]@{StartTime=$ownerBirth}}
    return $null
}
function Get-OptimizedMWQuickFreeBytes {param([string]$Path);return $script:freeBytes}
function New-QuickFixtureState([object]$Context){
    $id=[Guid]::NewGuid().ToString('N');$directory=Join-Path $Context.game_directory ('SystemTrace\capture '+$id)
    $null=New-Item -ItemType Directory -Path $directory
    return [pscustomobject]@{schema=2;coordinator_session_id=$Context.session_id;coordinator_process_id=$Context.owner_process_id;
        coordinator_creation_utc=$Context.owner_creation_utc;recorder_process_id=88;recorder_creation_utc=$helperBirth.ToString('o');
        capture_id=$id;instance=('OptimizedMW-'+$id);directory=$directory;game_directory=$Context.game_directory;
        executable_path=(Join-Path $Context.game_directory 'openmw.exe');executable_sha256=$Context.executable_sha256;
        status='recording';started=$true;normal_stop=$false;executable_unchanged=$true;etl_sha256=$null;etl_bytes=0;
        recording_started_utc=$ownerBirth.AddSeconds(2).ToString('o')}
}
function Start-OptimizedMWQuickRecorder {
    param([object]$Context,[int]$DurationSeconds)
    $script:helperCalls++
    Assert-Quick ($DurationSeconds -eq 600) 'Quick recorder default duration differs'
    if($script:startup -eq 'denied'){throw 'Mock UAC denied'}
    $script:helperExited=$false
    $helper=[pscustomobject]@{Id=88;StartTime=$helperBirth}
    $helper | Add-Member -MemberType ScriptMethod -Name Refresh -Value {}
    $helper | Add-Member -MemberType ScriptProperty -Name HasExited -Value {$script:helperExited}
    if($script:startup -ne 'no_ready'){
        $state=New-QuickFixtureState $Context
        if($script:startup -eq 'wrong_session'){$state.coordinator_session_id=[Guid]::NewGuid().ToString('N')}
        if($script:startup -eq 'stale_birth'){$state.recorder_creation_utc=$helperBirth.AddSeconds(-5).ToString('o')}
        if($script:startup -eq 'wrong_hash'){$state.executable_sha256=('f'*64)}
        if($script:startup -eq 'wrong_owner'){$state.coordinator_process_id=999}
        Write-OptimizedMWQuickJson (Join-Path $Context.directory 'RECORDER-STATE.json') $state
    }
    $script:lastStartContext=$Context
    return $helper
}
function New-QuickFixtureZip([string]$Path,[object[]]$Members){
    $stream=[IO.File]::Open($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    $writer=$null
    try{
        $writer=[IO.Compression.ZipArchive]::new($stream,[IO.Compression.ZipArchiveMode]::Create,$true)
        foreach($member in $Members){
            $entry=$writer.CreateEntry($member.name,[IO.Compression.CompressionLevel]::NoCompression)
            $output=$entry.Open()
            try{
                $bytes=$(if($member.bytes){[byte[]]$member.bytes}else{[Text.Encoding]::UTF8.GetBytes(($member.json | ConvertTo-Json -Depth 16))})
                $output.Write($bytes,0,$bytes.Length)
            }finally{$output.Dispose()}
        }
    }finally{if($writer){$writer.Dispose()};$stream.Dispose()}
}
function New-QuickFixturePair([object]$Context,[switch]$RecorderFirst){
    $state=$Context.trace_state | ConvertTo-Json -Depth 12 | ConvertFrom-Json
    $state.status='saved';$state.normal_stop=$true
    $etlBytes=[byte[]]@(3,1,4,1,5,9,2,6,5,3,5,8,9)
    $etlPath=Join-Path $state.directory 'OptimizedMW-SystemTrace.etl'
    [IO.File]::WriteAllBytes($etlPath,$etlBytes)
    $state.etl_sha256=(Get-FileHash -LiteralPath $etlPath).Hash.ToLowerInvariant();$state.etl_bytes=$etlBytes.Length
    Write-OptimizedMWQuickJson (Join-Path $state.directory 'SYSTEM-TRACE-STATE.json') $state
    Write-OptimizedMWQuickJson (Join-Path $Context.directory 'RECORDER-STATE.json') $state
    $link=[pscustomobject]@{schema=1;capture_id=$state.capture_id;instance=$state.instance;recorder_process_id=88;
        recorder_creation_utc=$helperBirth.ToString('o');directory=$state.directory;game_directory=$Context.game_directory;
        executable_path=$state.executable_path;executable_sha256=$state.executable_sha256}
    $engineZip=Join-Path $Context.directory 'fixture engine [one].zip'
    $systemZip=$state.directory+'.zip'
    if($RecorderFirst){
        New-QuickFixtureZip $systemZip @([pscustomobject]@{name='SYSTEM-TRACE-STATE.json';json=$state},[pscustomobject]@{name='OptimizedMW-SystemTrace.etl';bytes=$etlBytes})
    }
    New-QuickFixtureZip $engineZip @([pscustomobject]@{name='SYSTEM-TRACE-LINK.json';json=$link},[pscustomobject]@{name='raw.csv';bytes=([Text.Encoding]::UTF8.GetBytes("frame,ms`n1,16`n2,17`n"))})
    $binding=[pscustomobject]@{schema=1;capture_id=$state.capture_id;instance=$state.instance;recorder_process_id=88;
        recorder_creation_utc=$helperBirth.ToString('o');engine_zip_path=$engineZip;engine_zip_bytes=(Get-Item -LiteralPath $engineZip).Length;
        engine_zip_sha256=(Get-FileHash -LiteralPath $engineZip).Hash.ToLowerInvariant();game_process_id=99;
        game_creation_utc='2026-10-01T10:00:03.0000009Z'}
    Write-OptimizedMWQuickJson (Join-Path $state.directory 'SYSTEM-TRACE-ENGINE-BINDING.json') $binding
    if(-not $RecorderFirst){
        New-QuickFixtureZip $systemZip @([pscustomobject]@{name='SYSTEM-TRACE-STATE.json';json=$state},[pscustomobject]@{name='OptimizedMW-SystemTrace.etl';bytes=$etlBytes})
    }
    $recorder=[pscustomobject]@{schema=1;session_id=$Context.session_id;status='complete';error=$null;state=$state;system_zip=$systemZip}
    Write-OptimizedMWQuickJson (Join-Path $Context.directory 'RECORDER-RESULT.json') $recorder
    $script:helperExited=$true
    return [pscustomobject]@{engine=$engineZip;system=$systemZip;link=$link;binding=$binding;recorder=$recorder}
}
function Invoke-Phase9SystemTraceQualification {
    param([string]$GameDir,[string]$CaptureDirectory,[string]$ProfileZip)
    $script:qualifierCalls++
    if($script:qualifierMode -eq 'unavailable'){throw 'Mock native qualifier failure'}
    $state=Read-OptimizedMWQuickJson (Join-Path $CaptureDirectory 'SYSTEM-TRACE-STATE.json')
    $report=[pscustomobject]@{capture_id=$state.capture_id;scheduling_complete=($script:qualifierMode -eq 'complete');
        coverage=$(if($script:qualifierMode -eq 'complete'){'FULL_ENGINE_CLOCK_RANGE'}else{'PARTIAL'});
        engine_binding=[pscustomobject]@{identity_valid=$true;engine_zip_sha256=(Get-FileHash -LiteralPath $ProfileZip).Hash.ToLowerInvariant()}}
    if($script:qualifierMode -eq 'wrong_identity'){$report.capture_id=[Guid]::NewGuid().ToString('N')}
    if($script:qualifierMode -eq 'incomplete_native_identity'){
        $report.engine_binding.identity_valid=$false
        $report.scheduling_complete=$false
    }
    Write-OptimizedMWQuickJson ($ProfileZip+'.system-qualification.json') $report
}
try{
    $one=Get-OptimizedMWQuickPreset 1;$two=Get-OptimizedMWQuickPreset 2;$three=Get-OptimizedMWQuickPreset 3;$four=Get-OptimizedMWQuickPreset 4
    Assert-Quick ($one.mask -eq 0 -and $one.trace_level -eq 'Deep' -and $one.system_trace -and $one.candidate -eq 'CONTROL') 'Diagnosis control mapping changed'
    Assert-Quick ($two.mask -eq 0 -and $two.trace_level -eq 'Normal' -and -not $two.system_trace) 'Normal control mapping changed'
    Assert-Quick ($three.mask -eq 15 -and $three.trace_level -eq 'Normal' -and -not $three.system_trace) 'Normal all mapping changed'
    Assert-Quick ($four.mask -eq 15 -and $four.trace_level -eq 'Deep' -and $four.system_trace) 'Diagnosis all mapping changed'
    $normal=Start-OptimizedMWQuickSession $game $two
    Assert-Quick ($script:helperCalls -eq 0 -and -not $normal.system_requested -and $null -eq $normal.helper) 'Normal control requested elevation/recorder'
    $normalZip=Join-Path $normal.directory 'normal raw.zip';New-QuickFixtureZip $normalZip @([pscustomobject]@{name='frame.csv';bytes=([byte[]]@(1,2,3))})
    $normalHash=(Get-FileHash -LiteralPath $normalZip).Hash
    $result=Complete-OptimizedMWQuickSession -Context $normal -ProfileZip $normalZip -RestoreVerified $true -EnvironmentRestoreVerified $true
    Assert-Quick ($result.status -eq 'complete' -and $result.upload_path -eq $normalZip -and $script:qualifierCalls -eq 0 -and
        (Get-FileHash -LiteralPath $normalZip).Hash -eq $normalHash) 'Normal completion did system work or altered engine capture'
    Stop-OptimizedMWQuickSession $normal
    Assert-Quick (-not [IO.File]::Exists((Join-Path $normal.directory 'STOP-REQUEST.json'))) 'Normal completion wrote recorder cancellation'

    # A live manual recorder would contaminate an advertised Normal comparison.
    $activePath=Join-Path $game 'SYSTEM-TRACE-ACTIVE.json'
    Write-OptimizedMWQuickJson $activePath @{status='recording';recorder_process_id=77;recorder_creation_utc=$ownerBirth.ToString('o')}
    Assert-QuickThrows {Start-OptimizedMWQuickSession $game $two} 'Normal selection allowed a live recorder'
    Write-OptimizedMWQuickJson $activePath @{status='recording';recorder_process_id=77;recorder_creation_utc=$ownerBirth.AddSeconds(-1).ToString('o')}
    $stale=Start-OptimizedMWQuickSession $game $two
    Assert-Quick (-not $stale.system_requested -and $script:helperCalls -eq 0) 'Stale recorder metadata blocked a normal test'
    [IO.File]::Delete($activePath)

    $script:administrator=$true
    Assert-QuickThrows {Start-OptimizedMWQuickSession $game $one} 'Elevated coordinator was allowed to start diagnostic gameplay'
    Assert-QuickThrows {Start-OptimizedMWQuickSession $game $two} 'Elevated coordinator was allowed to start normal gameplay'
    $script:administrator=$false
    foreach($candidate in @('I1','I2','I1I2','D1')){
        $custom=[pscustomobject]@{mask=5;trace_level='Deep';candidate=$candidate;system_trace=$false;name='Custom'}
        $customContext=Start-OptimizedMWQuickSession $game $custom
        Assert-Quick ($customContext.selection.candidate -eq $candidate -and $customContext.selection.mask -eq 5 -and -not $customContext.system_requested) 'Custom candidate/combinations were rejected or changed'
    }
    $invalid=[pscustomobject]@{mask=5;trace_level='Normal';candidate='D1';system_trace=$false;name='Custom'}
    Assert-QuickThrows {Start-OptimizedMWQuickSession $game $invalid} 'D1 was accepted without deep telemetry'
    $script:startup='denied'
    Assert-QuickThrows {Start-OptimizedMWQuickSession $game $one} 'Declined elevation silently continued'
    foreach($mode in @('wrong_session','wrong_hash','stale_birth','wrong_owner')){
        $script:startup=$mode
        Assert-QuickThrows {Start-OptimizedMWQuickSession $game $one} ('Wrong ready identity was accepted: '+$mode)
        $stop=Read-OptimizedMWQuickJson (Join-Path $script:lastStartContext.directory 'STOP-REQUEST.json')
        Assert-Quick ($stop.session_id -eq $script:lastStartContext.session_id -and $stop.reason -eq 'coordinator_stop') 'Failed startup did not request cooperative owned cleanup'
    }
    $script:startup='ready'
    $context=Start-OptimizedMWQuickSession $game $one
    Assert-Quick ($context.trace_state.status -eq 'recording' -and $context.helper_process_id -eq 88) 'Recorder did not publish real readiness'
    $script:helperExited=$true
    Assert-QuickThrows {Assert-OptimizedMWQuickRecorderState $context $context.trace_state -Ready} 'Dead helper still qualified as ready'
    $script:helperExited=$false
    Stop-OptimizedMWQuickSession $context
    $stopPath=Join-Path $context.directory 'STOP-REQUEST.json';$stopHash=(Get-FileHash -LiteralPath $stopPath).Hash
    Stop-OptimizedMWQuickSession $context
    Assert-Quick ((Get-FileHash -LiteralPath $stopPath).Hash -eq $stopHash) 'Repeated stop request changed its identity'
    Assert-QuickThrows {Wait-OptimizedMWQuickRecorderFinished $context -TimeoutSeconds 1} 'Still-saving helper prematurely qualified as finished'
    Assert-Quick (-not [IO.File]::Exists((Join-Path $context.directory 'RECORDER-RESULT.json'))) 'Waiting invented a finished result'

    # Direct bounded readiness with no file avoids a 120-second test delay.
    $missing=$context | Select-Object *;$missing.directory=Join-Path $game ('TestSessions\'+[Guid]::NewGuid().ToString('N'))
    $missing.session_id=[IO.Path]::GetFileName($missing.directory);$null=New-Item -ItemType Directory -Path $missing.directory
    $missing.trace_state=$null
    Assert-QuickThrows {Wait-OptimizedMWQuickRecorderReady $missing -TimeoutSeconds 1} 'Missing ready handshake did not time out'

    foreach($recorderFirst in @($true,$false)){
        $context=Start-OptimizedMWQuickSession $game $one
        $pair=New-QuickFixturePair $context -RecorderFirst:$recorderFirst
        $engineHash=(Get-FileHash -LiteralPath $pair.engine).Hash;$systemHash=(Get-FileHash -LiteralPath $pair.system).Hash
        $result=Complete-OptimizedMWQuickSession -Context $context -ProfileZip $pair.engine -Link $pair.link -GameProcessId 99 `
            -GameCreationUtc $pair.binding.game_creation_utc -RestoreVerified $true -EnvironmentRestoreVerified $true
        Assert-Quick ($result.status -eq 'complete' -and $result.upload_path -ne $pair.engine -and [IO.File]::Exists($result.upload_path)) 'One-file upload did not finish in either save order'
        Assert-Quick ((Get-FileHash -LiteralPath $pair.engine).Hash -eq $engineHash -and (Get-FileHash -LiteralPath $pair.system).Hash -eq $systemHash) 'Combined packaging mutated original archives'
        $readback=[IO.Compression.ZipFile]::OpenRead($result.upload_path)
        try{
            Assert-Quick ($readback.Entries.Count -eq 4 -and $readback.GetEntry('ENGINE.zip') -and $readback.GetEntry('WINDOWS.zip') -and
                $readback.GetEntry('SYSTEM-QUALIFICATION.json') -and $readback.GetEntry('UPLOAD-MANIFEST.json')) 'Combined upload omitted final evidence'
            $manifest=Get-OptimizedMWQuickZipJson $readback 'UPLOAD-MANIFEST.json'
            Assert-Quick ($manifest.session_id -eq $context.session_id -and $manifest.capture_id -eq $context.trace_state.capture_id -and $manifest.files.Count -eq 3) 'Upload manifest lost exact pairing'
        }finally{$readback.Dispose()}
        Assert-QuickThrows {New-OptimizedMWQuickUploadZip $result.upload_path @()} 'Existing verified upload was replaced'
    }

    foreach($mode in @('partial','unavailable','incomplete_native_identity','wrong_identity')){
        $script:qualifierMode=$mode;$context=Start-OptimizedMWQuickSession $game $four;$pair=New-QuickFixturePair $context
        $engineHash=(Get-FileHash -LiteralPath $pair.engine).Hash;$systemHash=(Get-FileHash -LiteralPath $pair.system).Hash
        $result=Complete-OptimizedMWQuickSession -Context $context -ProfileZip $pair.engine -Link $pair.link -GameProcessId 99 `
            -GameCreationUtc $pair.binding.game_creation_utc -RestoreVerified $true -EnvironmentRestoreVerified $true
        Assert-Quick ($result.status -eq 'partial' -and $result.error) ('Unqualified run was claimed complete: '+$mode)
        Assert-Quick ((Get-FileHash -LiteralPath $pair.engine).Hash -eq $engineHash -and (Get-FileHash -LiteralPath $pair.system).Hash -eq $systemHash) 'Qualification failure changed original captures'
        if($mode -ne 'wrong_identity'){Assert-Quick ($result.upload_path -ne $pair.engine) 'Partial/unavailable qualification discarded otherwise exact evidence bundle'}
        else{Assert-Quick ($result.upload_path -eq $pair.engine) 'Wrong qualification identity entered an upload bundle'}
    }
    $script:qualifierMode='complete'
    $context=Start-OptimizedMWQuickSession $game $one;$pair=New-QuickFixturePair $context
    $beforeCalls=$script:qualifierCalls
    $different=$pair.link | Select-Object *;$different.capture_id=[Guid]::NewGuid().ToString('N')
    $result=Complete-OptimizedMWQuickSession -Context $context -ProfileZip $pair.engine -Link $different -GameProcessId 99 `
        -GameCreationUtc $pair.binding.game_creation_utc -RestoreVerified $true -EnvironmentRestoreVerified $true
    Assert-Quick ($result.status -eq 'partial' -and $result.upload_path -eq $pair.engine -and $script:qualifierCalls -eq $beforeCalls) 'Wrong capture ID was paired or qualified'
    Assert-QuickThrows {Assert-OptimizedMWQuickCapturePair $context $pair.recorder $pair.engine $pair.link 100 $pair.binding.game_creation_utc} 'Wrong game PID entered an upload'
    Assert-QuickThrows {Assert-OptimizedMWQuickCapturePair $context $pair.recorder $pair.engine $pair.link 99 '2026-10-01T10:00:04Z'} 'Wrong game birth entered an upload'
    $pair.binding.engine_zip_sha256=('f'*64)
    Write-OptimizedMWQuickJson (Join-Path $pair.recorder.state.directory 'SYSTEM-TRACE-ENGINE-BINDING.json') $pair.binding
    Assert-QuickThrows {Assert-OptimizedMWQuickCapturePair $context $pair.recorder $pair.engine $pair.link 99 $pair.binding.game_creation_utc} 'Wrong engine bytes entered an upload'

    $context=Start-OptimizedMWQuickSession $game $one;$pair=New-QuickFixturePair $context
    # Saved state/identity alone cannot hide truncated or changed ETL bytes.
    [IO.File]::Delete($pair.system)
    New-QuickFixtureZip $pair.system @([pscustomobject]@{name='SYSTEM-TRACE-STATE.json';json=$pair.recorder.state},
        [pscustomobject]@{name='OptimizedMW-SystemTrace.etl';bytes=([byte[]]@(8,9,5,3,5,6,2,9,5,1,4,1,3))})
    Assert-QuickThrows {Assert-OptimizedMWQuickCapturePair $context $pair.recorder $pair.engine $pair.link 99 $pair.binding.game_creation_utc} 'Same-length changed ETL bytes entered an upload'

    $context=Start-OptimizedMWQuickSession $game $one;$pair=New-QuickFixturePair $context
    $script:freeBytes=512MB
    $result=Complete-OptimizedMWQuickSession -Context $context -ProfileZip $pair.engine -Link $pair.link -GameProcessId 99 `
        -GameCreationUtc $pair.binding.game_creation_utc -RestoreVerified $true -EnvironmentRestoreVerified $true
    Assert-Quick ($result.status -eq 'partial' -and $result.upload_path -eq $pair.engine -and [IO.File]::Exists($pair.system)) 'Low disk lost raw capture or claimed combined upload'
    $script:freeBytes=100GB
    Assert-Quick (@(Get-ChildItem -LiteralPath $temp -Filter '*.tmp' -Recurse -File).Count -eq 0) 'Atomic packaging temporary files leaked'

    # Execute the actual thin helper in a child PowerShell with a stub recorder.
    # This covers its early-error result and proves its final result appears only
    # after recorder work ends, without elevation or any WPR invocation.
    foreach($name in @('OptimizedMW_QuickTest.ps1','OptimizedMW_QuickRecorder.ps1')){
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $game
    }
    $recorderStub=@'
param([string]$Mode,[int]$DurationSeconds,[string]$CoordinatorSessionId,[int]$CoordinatorProcessId,[string]$CoordinatorCreationUtc)
$directory=Join-Path $PSScriptRoot ('TestSessions\'+$CoordinatorSessionId)
if([IO.File]::Exists((Join-Path $directory 'MOCK-EARLY-ERROR'))){throw 'Mock recorder early preflight failure'}
$captureDirectory=Join-Path $PSScriptRoot ('SystemTrace\wrapper-'+$CoordinatorSessionId)
$null=New-Item -ItemType Directory -Path $captureDirectory
$state=@{schema=2;status='saved';normal_stop=$true;directory=$captureDirectory;coordinator_session_id=$CoordinatorSessionId;
    coordinator_process_id=$CoordinatorProcessId;coordinator_creation_utc=$CoordinatorCreationUtc}
Write-OptimizedMWQuickJson (Join-Path $directory 'RECORDER-STATE.json') $state
[IO.File]::WriteAllBytes(($captureDirectory+'.zip'),[byte[]]@(1,2,3))
[IO.File]::WriteAllText((Join-Path $directory 'MOCK-RECORDER-FINISHED'),'finished before helper result')
'@
    [IO.File]::WriteAllText((Join-Path $game 'OptimizedMW_SystemTrace.ps1'),$recorderStub)
    $hostExecutable=([Diagnostics.Process]::GetCurrentProcess()).MainModule.FileName
    foreach($earlyError in @($false,$true)){
        $wrapperContext=Start-OptimizedMWQuickSession $game $two
        if($earlyError){[IO.File]::WriteAllText((Join-Path $wrapperContext.directory 'MOCK-EARLY-ERROR'),'simulate preflight failure')}
        $startInfo=[Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName=$hostExecutable;$startInfo.UseShellExecute=$false;$startInfo.CreateNoWindow=$true
        $startInfo.Arguments=('-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "'+(Join-Path $game 'OptimizedMW_QuickRecorder.ps1')+
            '" -SessionId '+$wrapperContext.session_id+' -OwnerProcessId 77 -OwnerCreationUtc '+$ownerBirth.ToString('o'))
        $child=[Diagnostics.Process]::new();$child.StartInfo=$startInfo
        try{
            Assert-Quick $child.Start() 'Mock helper child did not start'
            Assert-Quick $child.WaitForExit(30000) 'Mock helper child timed out'
            $wrapperResult=Read-OptimizedMWQuickJson (Join-Path $wrapperContext.directory 'RECORDER-RESULT.json')
            Assert-Quick ($wrapperResult.session_id -eq $wrapperContext.session_id -and $wrapperResult.recorder_process_id -eq $child.Id) 'Actual helper result lost lifetime identity'
            if($earlyError){
                Assert-Quick ($child.ExitCode -eq 1 -and $wrapperResult.status -eq 'failed' -and $wrapperResult.error -match 'early preflight failure') 'Actual helper lost preflight failure result'
            }else{
                Assert-Quick ($child.ExitCode -eq 0 -and $wrapperResult.status -eq 'complete' -and
                    [IO.File]::Exists($wrapperResult.system_zip) -and [IO.File]::Exists((Join-Path $wrapperContext.directory 'MOCK-RECORDER-FINISHED'))) 'Actual helper published success before recorder work finished'
            }
        }finally{$child.Dispose()}
    }
    Write-Host 'PASS quick tests: four presets; Normal no elevation; active/stale recorder; elevated/UAC-denied controls; exact ready ID/birth/hash; cooperative stop; bounded wait; both completion orders; immutable paired single ZIP; partial/failing/wrong qualification; mismatched capture/game/bytes; low-disk preservation; nested spaces/brackets; no recording or game launched'
}finally{
    $resolved=[IO.Path]::GetFullPath($temp)
    Assert-Quick ($resolved.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase) -and $resolved -ne $tempRoot) 'Unsafe quick-test cleanup target'
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
