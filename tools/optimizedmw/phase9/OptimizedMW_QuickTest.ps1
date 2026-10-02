# The ordinary launcher coordinates elevation and immutable capture packaging.
# Only the recorder helper is elevated; this library never starts the game.
function Get-OptimizedMWQuickPreset {
    param([ValidateSet('1','2','3','4','5','6','7','8','9','10','11')][string]$Choice)
    switch($Choice){
        '1'{return [pscustomobject]@{mask=0;trace_level='Deep';candidate='CONTROL';system_trace=$true;name='Diagnose CONTROL'}}
        '2'{return [pscustomobject]@{mask=0;trace_level='Normal';candidate='CONTROL';system_trace=$false;name='Normal CONTROL'}}
        '3'{return [pscustomobject]@{mask=15;trace_level='Normal';candidate='CONTROL';system_trace=$false;name='Normal ALL'}}
        '4'{return [pscustomobject]@{mask=15;trace_level='Deep';candidate='CONTROL';system_trace=$true;name='Diagnose ALL'}}
        '5'{return [pscustomobject]@{mask=4;attack_mask=0;trace_level='Normal';candidate='CONTROL';system_trace=$false;name='Normal ANIM control'}}
        '6'{return [pscustomobject]@{mask=4;attack_mask=1;trace_level='Normal';candidate='CONTROL';system_trace=$false;name='Normal ANIM retained'}}
        '7'{return [pscustomobject]@{mask=4;attack_mask=1;trace_level='Deep';candidate='CONTROL';system_trace=$true;name='Diagnose ANIM retained'}}
        '8'{return [pscustomobject]@{mask=4;attack_mask=129;trace_level='Normal';candidate='CONTROL';system_trace=$false;name='Normal ANIM repaired'}}
        '9'{return [pscustomobject]@{mask=4;attack_mask=129;trace_level='Deep';candidate='CONTROL';system_trace=$true;gpu_query_defer=$true;name='Diagnose ANIM repaired'}}
        '10'{return [pscustomobject]@{mask=4;attack_mask=449;trace_level='Normal';candidate='CONTROL';system_trace=$false;name='Normal repair combination'}}
        '11'{return [pscustomobject]@{mask=4;attack_mask=449;trace_level='Deep';candidate='CONTROL';system_trace=$true;gpu_query_defer=$true;name='Diagnose repair combination'}}
    }
}

function ConvertTo-OptimizedMWQuickUtc {
    param([object]$Value)
    if($Value -is [DateTime]){return $Value.ToUniversalTime().ToString('o')}
    return [DateTime]::Parse([string]$Value).ToUniversalTime().ToString('o')
}

function Write-OptimizedMWQuickJson {
    param([string]$Path,[object]$Value,[switch]$CreateOnly)
    $temporary=$Path+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
    try{
        $Value | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $temporary -Encoding UTF8
        if([IO.File]::Exists($Path)){
            if($CreateOnly){throw 'Preserving an existing quick-session record'}
            [IO.File]::Replace($temporary,$Path,[System.Management.Automation.Language.NullString]::Value)
        }else{[IO.File]::Move($temporary,$Path)}
    }finally{if([IO.File]::Exists($temporary)){[IO.File]::Delete($temporary)}}
}

function Read-OptimizedMWQuickJson {
    param([string]$Path)
    $file=Get-Item -LiteralPath $Path -ErrorAction Stop
    if($file.Length -le 0 -or $file.Length -gt 4MB -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)){
        throw 'Quick-session metadata is empty, oversized or a reparse point'
    }
    return Get-Content -LiteralPath $Path -Raw -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop
}

function Test-OptimizedMWQuickAdministrator {
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-OptimizedMWQuickOwner {
    $process=Get-Process -Id $PID -ErrorAction Stop
    return [pscustomobject]@{process_id=$PID;creation_utc=$process.StartTime.ToUniversalTime().ToString('o')}
}

function Assert-OptimizedMWQuickNoActiveRecorder {
    param([string]$GameDirectory)
    $path=Join-Path $GameDirectory 'SYSTEM-TRACE-ACTIVE.json'
    if(-not [IO.File]::Exists($path)){return}
    $active=Read-OptimizedMWQuickJson $path
    if($active.status -notin @('starting','recording','saving')){return}
    $process=Get-Process -Id ([int]$active.recorder_process_id) -ErrorAction SilentlyContinue
    if($process -and $process.StartTime.ToUniversalTime().ToString('o') -eq (ConvertTo-OptimizedMWQuickUtc $active.recorder_creation_utc)){
        throw 'A Windows recorder is still active for this test folder. Let its capture finish before choosing a new test; Normal tests require the recorder to be off.'
    }
}

function Get-OptimizedMWQuickPowerShell {
    $systemDirectory=$(if([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess){'Sysnative'}else{'System32'})
    $executable=Join-Path $env:WINDIR ($systemDirectory+'\WindowsPowerShell\v1.0\powershell.exe')
    if(-not [IO.File]::Exists($executable)){throw 'Windows PowerShell is unavailable for the recorder helper'}
    return $executable
}

function Start-OptimizedMWQuickRecorder {
    param([object]$Context,[int]$DurationSeconds)
    $scriptPath=Join-Path $Context.game_directory 'OptimizedMW_QuickRecorder.ps1'
    if(-not [IO.File]::Exists($scriptPath)){throw 'The automatic system recorder helper is missing'}
    # Windows filenames cannot contain double quotes. Every supplied parameter
    # is bounded or fixed; elevation never receives a string-built shell script.
    if($scriptPath.Contains('"')){throw 'Unsupported quoted recorder path'}
    $arguments=@('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',('"'+$scriptPath+'"'),
        '-SessionId',$Context.session_id,'-OwnerProcessId',[string]$Context.owner_process_id,
        '-OwnerCreationUtc',$Context.owner_creation_utc,'-DurationSeconds',[string]$DurationSeconds)
    return Start-Process -FilePath (Get-OptimizedMWQuickPowerShell) -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -PassThru
}

function Test-OptimizedMWQuickHelperExited {
    param([object]$Context)
    if($null -eq $Context.helper){return $true}
    try{$Context.helper.Refresh();return [bool]$Context.helper.HasExited}catch{
        $process=Get-Process -Id $Context.helper_process_id -ErrorAction SilentlyContinue
        if(-not $process){return $true}
        return $process.StartTime.ToUniversalTime().ToString('o') -ne $Context.helper_creation_utc
    }
}

function Assert-OptimizedMWQuickContext {
    param([object]$Context)
    if($null -eq $Context -or $Context.session_id -notmatch '^[0-9a-fA-F]{32}$'){throw 'Invalid quick-session identity'}
    $expected=[IO.Path]::GetFullPath((Join-Path $Context.game_directory ('TestSessions\'+$Context.session_id)))
    if(-not [string]::Equals($expected,[IO.Path]::GetFullPath($Context.directory),[StringComparison]::OrdinalIgnoreCase)){
        throw 'Quick-session directory does not match its exact identity'
    }
}

function Assert-OptimizedMWQuickRecorderState {
    param([object]$Context,[object]$State,[switch]$Ready)
    Assert-OptimizedMWQuickContext $Context
    if($State.schema -ne 2 -or $State.coordinator_session_id -ne $Context.session_id -or
        $State.coordinator_process_id -ne $Context.owner_process_id -or
        (ConvertTo-OptimizedMWQuickUtc $State.coordinator_creation_utc) -ne $Context.owner_creation_utc -or
        $State.recorder_process_id -ne $Context.helper_process_id -or
        (ConvertTo-OptimizedMWQuickUtc $State.recorder_creation_utc) -ne $Context.helper_creation_utc -or
        $State.capture_id -notmatch '^[0-9a-fA-F]{32}$' -or $State.instance -ne ('OptimizedMW-'+$State.capture_id)){
        throw 'Recorder metadata belongs to a different session or process lifetime'
    }
    $executable=[IO.Path]::GetFullPath((Join-Path $Context.game_directory 'openmw.exe'))
    if(-not [string]::Equals([IO.Path]::GetFullPath($State.executable_path),$executable,[StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals([IO.Path]::GetFullPath($State.game_directory),$Context.game_directory,[StringComparison]::OrdinalIgnoreCase) -or
        $State.executable_sha256 -ne $Context.executable_sha256){throw 'Recorder executable identity differs from this test'}
    $traceRoot=[IO.Path]::GetFullPath((Join-Path $Context.game_directory 'SystemTrace')).TrimEnd('\','/')+'\'
    $traceDirectory=[IO.Path]::GetFullPath($State.directory).TrimEnd('\','/')+'\'
    if(-not $traceDirectory.StartsWith($traceRoot,[StringComparison]::OrdinalIgnoreCase) -or $traceDirectory -eq $traceRoot){
        throw 'Recorder capture directory is outside this game SystemTrace directory'
    }
    if($Context.trace_state -and $State.capture_id -ne $Context.trace_state.capture_id){throw 'Recorder capture changed after readiness'}
    if($Ready){
        if($State.status -ne 'recording' -or -not $State.started -or $State.normal_stop -or
            (Test-OptimizedMWQuickHelperExited $Context)){throw 'The requested system recorder is not live and recording'}
        if((Get-FileHash -LiteralPath $executable -ErrorAction Stop).Hash -ne $Context.executable_sha256){throw 'Game executable changed during recorder startup'}
    }
}

function Wait-OptimizedMWQuickRecorderReady {
    param([object]$Context,[ValidateRange(1,180)][int]$TimeoutSeconds=120)
    $watch=[Diagnostics.Stopwatch]::StartNew();$nextUpdate=30
    $statePath=Join-Path $Context.directory 'RECORDER-STATE.json'
    $resultPath=Join-Path $Context.directory 'RECORDER-RESULT.json'
    while($watch.Elapsed.TotalSeconds -lt $TimeoutSeconds){
        if([IO.File]::Exists($resultPath)){
            $result=Read-OptimizedMWQuickJson $resultPath
            if($result.session_id -ne $Context.session_id){throw 'Recorder result belongs to another session'}
            throw ('System recorder ended before readiness: '+$result.error)
        }
        if([IO.File]::Exists($statePath)){
            $state=Read-OptimizedMWQuickJson $statePath
            Assert-OptimizedMWQuickRecorderState $Context $state
            if($state.status -eq 'recording'){
                Assert-OptimizedMWQuickRecorderState $Context $state -Ready
                return $state
            }
            if($state.status -notin @('starting')){throw ('System recorder did not become ready: '+$state.status)}
        }
        if(Test-OptimizedMWQuickHelperExited $Context){throw 'System recorder exited without a ready handshake'}
        if($watch.Elapsed.TotalSeconds -ge $nextUpdate){Write-Host 'Waiting for Windows recording to start...';$nextUpdate+=30}
        Start-Sleep -Milliseconds 250
    }
    throw "System recorder was not ready within $TimeoutSeconds seconds; no game was launched"
}

function Start-OptimizedMWQuickSession {
    param([Parameter(Mandatory=$true)][string]$GameDirectory,[Parameter(Mandatory=$true)][object]$Selection,
        [ValidateRange(30,1800)][int]$DurationSeconds=600)
    if($Selection.mask -lt 0 -or $Selection.mask -gt 15 -or $Selection.trace_level -notin @('Normal','Deep') -or
        $Selection.candidate -notin @('CONTROL','I1','I2','I1I2','D1') -or
        ($Selection.candidate -eq 'D1' -and $Selection.trace_level -ne 'Deep') -or
        ($Selection.system_trace -and $Selection.trace_level -ne 'Deep')){
        throw 'Invalid quick-test selection'
    }
    $game=[IO.Path]::GetFullPath($GameDirectory).TrimEnd('\','/')
    Assert-OptimizedMWQuickNoActiveRecorder $game
    if(Test-OptimizedMWQuickAdministrator){
        throw 'Open the test launcher normally. It requests administrator permission only for the Windows recorder; the game must run with normal permissions.'
    }
    $owner=Get-OptimizedMWQuickOwner
    $id=[Guid]::NewGuid().ToString('N');$directory=Join-Path $game ('TestSessions\'+$id)
    $null=New-Item -ItemType Directory -Path $directory -ErrorAction Stop
    $context=[pscustomobject]@{session_id=$id;directory=$directory;game_directory=$game;selection=$Selection;
        system_requested=[bool]$Selection.system_trace;owner_process_id=$owner.process_id;owner_creation_utc=$owner.creation_utc;
        executable_sha256=$null;helper=$null;helper_process_id=0;helper_creation_utc=$null;trace_state=$null}
    Write-OptimizedMWQuickJson (Join-Path $directory 'SESSION-REQUEST.json') ([ordered]@{schema=1;session_id=$id;
        owner_process_id=$owner.process_id;owner_creation_utc=$owner.creation_utc;selection=$Selection;
        duration_seconds=$DurationSeconds;requested_utc=[DateTime]::UtcNow.ToString('o')}) -CreateOnly
    if(-not $context.system_requested){return $context}
    try{
        $context.executable_sha256=(Get-FileHash -LiteralPath (Join-Path $game 'openmw.exe') -ErrorAction Stop).Hash.ToLowerInvariant()
        Write-Host 'Approve the Windows recorder prompt. OpenMW will start automatically once recording is ready.' -ForegroundColor Cyan
        $context.helper=Start-OptimizedMWQuickRecorder $context $DurationSeconds
        $context.helper_process_id=$context.helper.Id
        $context.helper_creation_utc=$context.helper.StartTime.ToUniversalTime().ToString('o')
        $context.trace_state=Wait-OptimizedMWQuickRecorderReady $context
        Write-Host 'Windows recording is ready; starting the selected game test.' -ForegroundColor Green
        return $context
    }catch{
        $failure=[string]$_
        if($context.helper){Stop-OptimizedMWQuickSession $context}
        Write-OptimizedMWQuickJson (Join-Path $directory 'SESSION-RESULT.json') ([ordered]@{schema=1;session_id=$id;
            status='startup_failed';error=$failure;raw_session_directory=$directory;upload_path=$null})
        throw
    }
}

function Stop-OptimizedMWQuickSession {
    param([object]$Context)
    if($null -eq $Context -or -not $Context.system_requested){return}
    Assert-OptimizedMWQuickContext $Context
    # Cooperative request. Only the recorder with this session ID can act on it;
    # never kill the game/helper or cancel an unnamed WPR session.
    $path=Join-Path $Context.directory 'STOP-REQUEST.json'
    if(-not [IO.File]::Exists($path)){
        Write-OptimizedMWQuickJson $path ([ordered]@{schema=1;session_id=$Context.session_id;
            requested_utc=[DateTime]::UtcNow.ToString('o');reason='coordinator_stop'}) -CreateOnly
    }
}

function Wait-OptimizedMWQuickRecorderFinished {
    param([object]$Context,[ValidateRange(1,1800)][int]$TimeoutSeconds=900)
    $watch=[Diagnostics.Stopwatch]::StartNew();$nextUpdate=30
    while(-not (Test-OptimizedMWQuickHelperExited $Context)){
        if($watch.Elapsed.TotalSeconds -ge $TimeoutSeconds){throw 'Windows recorder is still saving; raw captures are retained. Do not terminate its helper.'}
        if($watch.Elapsed.TotalSeconds -ge $nextUpdate){Write-Host 'Still saving and verifying the Windows capture. Please leave this launcher open.';$nextUpdate+=30}
        Start-Sleep -Milliseconds 250
    }
    $path=Join-Path $Context.directory 'RECORDER-RESULT.json'
    if(-not [IO.File]::Exists($path)){throw 'Windows recorder ended without a final result; raw capture directory is retained'}
    $result=Read-OptimizedMWQuickJson $path
    if($result.schema -ne 1 -or $result.session_id -ne $Context.session_id){throw 'Final recorder result belongs to another session'}
    if($result.state){Assert-OptimizedMWQuickRecorderState $Context $result.state}
    if($result.status -ne 'complete'){throw ('Windows recorder did not finish: '+$result.error)}
    return $result
}

function Get-OptimizedMWQuickZipJson {
    param([IO.Compression.ZipArchive]$Archive,[string]$Name)
    $entries=@($Archive.Entries | Where-Object {$_.FullName.Replace('\','/') -eq $Name})
    if($entries.Count -ne 1 -or $entries[0].Length -gt 4MB){throw ('Capture ZIP lacks unique bounded '+$Name)}
    $reader=[IO.StreamReader]::new($entries[0].Open())
    try{return $reader.ReadToEnd() | ConvertFrom-Json -ErrorAction Stop}finally{$reader.Dispose()}
}

function Get-OptimizedMWQuickStreamHash {
    param([IO.Stream]$Stream)
    $hasher=[Security.Cryptography.SHA256]::Create()
    try{return [BitConverter]::ToString($hasher.ComputeHash($Stream)).Replace('-','').ToLowerInvariant()}finally{$hasher.Dispose()}
}

function Assert-OptimizedMWQuickCapturePair {
    param([object]$Context,[object]$RecorderResult,[string]$ProfileZip,[object]$Link,[int]$GameProcessId,[string]$GameCreationUtc)
    if(-not $Link -or $GameProcessId -le 0){throw 'System test has no exact final engine link/process identity'}
    Assert-OptimizedMWQuickRecorderState $Context $RecorderResult.state
    $state=$RecorderResult.state
    if(-not $state.normal_stop -or $state.status -ne 'saved' -or -not $state.executable_unchanged){throw 'System recorder has no saved unchanged-executable capture'}
    if($Link.capture_id -ne $state.capture_id -or $Link.instance -ne $state.instance -or
        $Link.recorder_process_id -ne $state.recorder_process_id -or
        (ConvertTo-OptimizedMWQuickUtc $Link.recorder_creation_utc) -ne (ConvertTo-OptimizedMWQuickUtc $state.recorder_creation_utc) -or
        $Link.executable_sha256 -ne $state.executable_sha256 -or
        -not [string]::Equals([IO.Path]::GetFullPath($Link.directory),[IO.Path]::GetFullPath($state.directory),[StringComparison]::OrdinalIgnoreCase)){
        throw 'Final engine link belongs to another system capture'
    }
    $systemZip=[IO.Path]::GetFullPath([string]$RecorderResult.system_zip)
    if(-not [string]::Equals($systemZip,[IO.Path]::GetFullPath($state.directory+'.zip'),[StringComparison]::OrdinalIgnoreCase)){
        throw 'Recorder result selected a different system archive'
    }
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $engine=[IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($ProfileZip))
    try{
        $archived=Get-OptimizedMWQuickZipJson $engine 'SYSTEM-TRACE-LINK.json'
        foreach($field in @('capture_id','instance','recorder_process_id','recorder_creation_utc','executable_path','executable_sha256','directory')){
            $actual=$archived.$field;$expected=$Link.$field
            if($field -eq 'recorder_creation_utc'){$actual=ConvertTo-OptimizedMWQuickUtc $actual;$expected=ConvertTo-OptimizedMWQuickUtc $expected}
            if($actual -ne $expected){throw ('Archived engine link mismatch: '+$field)}
        }
    }finally{$engine.Dispose()}
    $binding=Read-OptimizedMWQuickJson (Join-Path $state.directory 'SYSTEM-TRACE-ENGINE-BINDING.json')
    if($binding.schema -ne 1 -or $binding.capture_id -ne $state.capture_id -or $binding.instance -ne $state.instance -or
        $binding.recorder_process_id -ne $state.recorder_process_id -or
        (ConvertTo-OptimizedMWQuickUtc $binding.recorder_creation_utc) -ne (ConvertTo-OptimizedMWQuickUtc $state.recorder_creation_utc) -or
        $binding.game_process_id -ne $GameProcessId -or
        (ConvertTo-OptimizedMWQuickUtc $binding.game_creation_utc) -ne (ConvertTo-OptimizedMWQuickUtc $GameCreationUtc) -or
        -not [string]::Equals([IO.Path]::GetFullPath($binding.engine_zip_path),[IO.Path]::GetFullPath($ProfileZip),[StringComparison]::OrdinalIgnoreCase) -or
        $binding.engine_zip_bytes -ne (Get-Item -LiteralPath $ProfileZip).Length -or
        $binding.engine_zip_sha256 -ne (Get-FileHash -LiteralPath $ProfileZip).Hash){throw 'Final engine binding is missing or belongs to different bytes/process'}
    $system=[IO.Compression.ZipFile]::OpenRead($systemZip)
    try{
        $archivedState=Get-OptimizedMWQuickZipJson $system 'SYSTEM-TRACE-STATE.json'
        Assert-OptimizedMWQuickRecorderState $Context $archivedState
        if($archivedState.status -ne 'saved' -or -not $archivedState.normal_stop -or $archivedState.etl_sha256 -ne $state.etl_sha256 -or
            $archivedState.etl_bytes -ne $state.etl_bytes){throw 'Archived system state differs from the saved recorder result'}
        $etls=@($system.Entries | Where-Object {$_.FullName -eq 'OptimizedMW-SystemTrace.etl'})
        if($etls.Count -ne 1 -or $etls[0].Length -le 0 -or $etls[0].Length -ne $state.etl_bytes){throw 'System ZIP lacks its complete unique ETL'}
        $stream=$etls[0].Open()
        try{if((Get-OptimizedMWQuickStreamHash $stream) -ne $state.etl_sha256){throw 'Archived ETL hash differs from recorder state'}}finally{$stream.Dispose()}
    }finally{$system.Dispose()}
    return $binding
}

function Get-OptimizedMWQuickFreeBytes {
    param([string]$Path)
    return ([IO.DriveInfo]::new([IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Path)))).AvailableFreeSpace
}

function New-OptimizedMWQuickUploadZip {
    param([string]$DestinationPath,[object[]]$Files)
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $destination=[IO.Path]::GetFullPath($DestinationPath)
    if([IO.File]::Exists($destination)){throw 'Preserving an existing verified upload bundle'}
    $inventory=@($Files | ForEach-Object {
        $file=Get-Item -LiteralPath $_.path -ErrorAction Stop
        if(($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $_.name -match '[/\\]' -or $_.name -eq ''){throw 'Invalid upload member path'}
        [pscustomobject]@{path=$file.FullName;name=$_.name;bytes=$file.Length;sha256=(Get-FileHash -LiteralPath $file.FullName).Hash.ToLowerInvariant()}
    })
    $total=0L;foreach($file in $inventory){$total+=$file.bytes}
    if((Get-OptimizedMWQuickFreeBytes ([IO.Path]::GetDirectoryName($destination))) -lt ($total+1GB)){
        throw 'Insufficient free space for an additional upload bundle; the original engine/system captures are retained'
    }
    $temporary=$destination+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
    try{
        $output=[IO.File]::Open($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
        $writer=$null
        try{
            $writer=[IO.Compression.ZipArchive]::new($output,[IO.Compression.ZipArchiveMode]::Create,$true)
            $names=@{}
            foreach($file in $inventory){
                if($names.ContainsKey($file.name)){throw 'Duplicate upload bundle member'};$names[$file.name]=$true
                $level=$(if($file.name.EndsWith('.zip',[StringComparison]::OrdinalIgnoreCase)){[IO.Compression.CompressionLevel]::NoCompression}else{[IO.Compression.CompressionLevel]::Optimal})
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($writer,$file.path,$file.name,$level)
            }
        }finally{if($writer){$writer.Dispose()};$output.Dispose()}
        $readback=[IO.Compression.ZipFile]::OpenRead($temporary)
        try{
            if($readback.Entries.Count -ne $inventory.Count){throw 'Upload bundle inventory differs from original captures'}
            foreach($file in $inventory){
                $entries=@($readback.Entries | Where-Object {$_.FullName -eq $file.name})
                if($entries.Count -ne 1 -or $entries[0].Length -ne $file.bytes){throw ('Missing/truncated upload member: '+$file.name)}
                $stream=$entries[0].Open()
                try{if((Get-OptimizedMWQuickStreamHash $stream) -ne $file.sha256){throw ('Upload member hash mismatch: '+$file.name)}}finally{$stream.Dispose()}
                if((Get-FileHash -LiteralPath $file.path).Hash -ne $file.sha256){throw ('Source capture changed during packaging: '+$file.name)}
            }
        }finally{$readback.Dispose()}
        [IO.File]::Move($temporary,$destination)
        return [pscustomobject]@{path=$destination;sha256=(Get-FileHash -LiteralPath $destination).Hash.ToLowerInvariant();files=$inventory}
    }finally{if([IO.File]::Exists($temporary)){[IO.File]::Delete($temporary)}}
}

function Complete-OptimizedMWQuickSession {
    param([object]$Context,[string]$ProfileZip,[object]$Link,[int]$GameProcessId,[string]$GameCreationUtc,
        [bool]$RestoreVerified,[bool]$EnvironmentRestoreVerified,[object]$LauncherError)
    if($null -eq $Context){return $null}
    Assert-OptimizedMWQuickContext $Context
    $result=[ordered]@{schema=1;session_id=$Context.session_id;selection=$Context.selection;status='complete';
        engine_zip=[IO.Path]::GetFullPath($ProfileZip);engine_zip_sha256=(Get-FileHash -LiteralPath $ProfileZip).Hash.ToLowerInvariant();
        system_requested=$Context.system_requested;system_zip=$null;upload_path=[IO.Path]::GetFullPath($ProfileZip);
        settings_restore_verified=$RestoreVerified;environment_restore_verified=$EnvironmentRestoreVerified;
        error=$null;qualification=$null;completed_utc=$null;raw_session_directory=$Context.directory}
    if(-not $RestoreVerified -or -not $EnvironmentRestoreVerified -or $LauncherError){
        $result.status='partial';$result.error='Launcher/restoration did not complete cleanly: '+[string]$LauncherError
    }
    if($Context.system_requested){
        try{
            Write-Host 'Saving the combined engine and Windows capture. Leave this launcher open.' -ForegroundColor Cyan
            $recorder=Wait-OptimizedMWQuickRecorderFinished $Context
            $null=Assert-OptimizedMWQuickCapturePair $Context $recorder $ProfileZip $Link $GameProcessId $GameCreationUtc
            $result.system_zip=[IO.Path]::GetFullPath([string]$recorder.system_zip)
            # Requalify AFTER final binding and helper exit. Recorder and engine
            # can finish in either order; their already verified ZIPs stay intact.
            $qualificationError=$null
            try{Invoke-Phase9SystemTraceQualification $Context.game_directory $recorder.state.directory $ProfileZip}catch{$qualificationError=[string]$_}
            $companion=$ProfileZip+'.system-qualification.json'
            $qualification=$null
            if([IO.File]::Exists($companion)){
                $qualification=Read-OptimizedMWQuickJson $companion
                if($qualification.capture_id -ne $recorder.state.capture_id -or
                    $qualification.engine_binding.engine_zip_sha256 -ne $result.engine_zip_sha256){
                    throw 'Final qualification companion does not identify this immutable engine capture'
                }
                $result.qualification=$qualification
            }
            # Native clock/inventory identity can be incomplete after a crash.
            # Keep its explicit invalid status in a partial upload; the raw ZIP
            # pairing was independently proven by link and immutable binding.
            if($qualificationError -or -not $qualification -or -not $qualification.scheduling_complete -or
                -not $qualification.engine_binding.identity_valid){
                $result.status='partial';$result.error=(@($result.error,$qualificationError,'Windows qualification is partial or unavailable') | Where-Object {$_}) -join '; '
            }
            $manifest=[ordered]@{schema=1;session_id=$Context.session_id;capture_id=$recorder.state.capture_id;
                selection=$Context.selection;game_process_id=$GameProcessId;game_creation_utc=(ConvertTo-OptimizedMWQuickUtc $GameCreationUtc);
                status=$result.status;qualification_error=$qualificationError;settings_restore_verified=$RestoreVerified;
                environment_restore_verified=$EnvironmentRestoreVerified;launcher_error=[string]$LauncherError;
                files=@([pscustomobject]@{name='ENGINE.zip';sha256=$result.engine_zip_sha256;bytes=(Get-Item -LiteralPath $ProfileZip).Length},
                    [pscustomobject]@{name='WINDOWS.zip';sha256=(Get-FileHash -LiteralPath $result.system_zip).Hash.ToLowerInvariant();bytes=(Get-Item -LiteralPath $result.system_zip).Length});
                note='Nested original archives are unchanged. Hash pairing is not proof of complete telemetry, causality or runtime improvement.'}
            $manifestPath=Join-Path $Context.directory 'UPLOAD-MANIFEST.json'
            $files=@([pscustomobject]@{name='ENGINE.zip';path=$ProfileZip},[pscustomobject]@{name='WINDOWS.zip';path=$result.system_zip})
            if($qualification){
                $files+=,[pscustomobject]@{name='SYSTEM-QUALIFICATION.json';path=$companion}
                $manifest.files+=,[pscustomobject]@{name='SYSTEM-QUALIFICATION.json';sha256=(Get-FileHash -LiteralPath $companion).Hash.ToLowerInvariant();bytes=(Get-Item -LiteralPath $companion).Length}
            }
            Write-OptimizedMWQuickJson $manifestPath $manifest -CreateOnly
            $files+=,[pscustomobject]@{name='UPLOAD-MANIFEST.json';path=$manifestPath}
            $destination=Join-Path $Context.game_directory ('OptimizedMW_UPLOAD_'+$Context.session_id+'.zip')
            $bundle=New-OptimizedMWQuickUploadZip $destination $files
            $result.upload_path=$bundle.path
            $result.upload_sha256=$bundle.sha256
            Write-Host ('Upload this one verified file: '+$bundle.path) -ForegroundColor Green
        }catch{
            $result.status='partial';$result.error=([string]$result.error+'; '+[string]$_).Trim([char[]]'; ')
            Write-Warning ('Combined upload unavailable: '+$_+'. Original captures remain intact.')
        }
    }
    $result.completed_utc=[DateTime]::UtcNow.ToString('o')
    Write-OptimizedMWQuickJson (Join-Path $Context.directory 'SESSION-RESULT.json') $result
    return [pscustomobject]$result
}
