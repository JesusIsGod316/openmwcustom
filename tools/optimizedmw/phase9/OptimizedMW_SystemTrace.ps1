param(
    [ValidateSet('File','Memory')][string]$Mode='File',
    [ValidateRange(30,1800)][int]$DurationSeconds=360,
    [ValidateRange(2,100)][int]$MinimumFreeGiB=10,
    [switch]$ValidateOnly,
    [switch]$FunctionsOnly
)
$ErrorActionPreference='Stop'

function ConvertTo-SystemTraceUtcText {
    param([object]$Value)
    if($Value -is [DateTime]){return $Value.ToUniversalTime().ToString('o')}
    return [DateTime]::Parse([string]$Value).ToUniversalTime().ToString('o')
}

function Invoke-SystemRecorder {
    param([string[]]$Arguments)
    # PS5 wraps native stderr in ErrorRecords. Always observe the actual exit
    # code, so a session which started successfully acquires cleanup ownership.
    $ErrorActionPreference='Continue'
    $lines=@(& $script:wpr @Arguments 2>&1)
    return [pscustomobject]@{Code=$LASTEXITCODE;Lines=@($lines | ForEach-Object {[string]$_})}
}

function Get-SystemTraceFreeBytes {
    param([string]$Path)
    $drive=[IO.DriveInfo]::new([IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Path)))
    return $drive.AvailableFreeSpace
}

function Get-SystemTraceTemporaryBytes {
    param([string]$Path)
    $bytes=0L
    foreach($file in @(Get-ChildItem -LiteralPath $Path -Recurse -File)){$bytes+=$file.Length}
    return $bytes
}

function New-VerifiedSystemTraceZip {
    param([Parameter(Mandatory=$true)][string]$SourceDirectory,
          [Parameter(Mandatory=$true)][string]$DestinationPath)
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $source=[IO.Path]::GetFullPath($SourceDirectory).TrimEnd('\','/')
    $destination=[IO.Path]::GetFullPath($DestinationPath)
    if($destination.StartsWith($source+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){
        throw 'System ZIP must be outside its captured directory'
    }
    if([IO.File]::Exists($destination)){throw 'Existing verified system ZIP must be preserved'}
    foreach($name in @('OptimizedMW-SystemTrace.etl','SYSTEM-TRACE-STATE.json')){
        if(-not [IO.File]::Exists((Join-Path $source $name))){throw ('System capture is missing '+$name)}
    }
    $temp=$destination+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
    $lastError=$null
    for($attempt=1;$attempt -le 3;$attempt++){
        try{
            # Only final top-level files are shared. The WPRTemp directory and
            # concurrent atomic-write temporary files never enter the archive.
            $files=@(Get-ChildItem -LiteralPath $source -File | Where-Object {-not $_.Name.EndsWith('.tmp')})
            $inventory=@($files | ForEach-Object {
                if(($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw 'Refusing to archive a reparse-point file'}
                [pscustomobject]@{name=$_.Name;bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
            })
            $state=Get-Content -Raw -LiteralPath (Join-Path $source 'SYSTEM-TRACE-STATE.json') | ConvertFrom-Json
            $etl=@($inventory | Where-Object {$_.name -eq 'OptimizedMW-SystemTrace.etl'})[0]
            if(-not $state.normal_stop -or $state.status -ne 'saved' -or $etl.bytes -le 0 -or $state.etl_sha256 -ne $etl.sha256){
                throw 'Only a saved nonempty ETL matching its recorder hash can be archived'
            }
            $output=[IO.File]::Open($temp,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
            $writer=$null
            try{
                $writer=[IO.Compression.ZipArchive]::new($output,[IO.Compression.ZipArchiveMode]::Create,$true)
                foreach($file in $inventory){
                    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($writer,(Join-Path $source $file.name),$file.name,[IO.Compression.CompressionLevel]::Optimal)
                }
            }finally{if($writer){$writer.Dispose()};$output.Dispose()}
            $archive=[IO.Compression.ZipFile]::OpenRead($temp)
            try{
                $entries=@{}
                foreach($entry in $archive.Entries){
                    if($entries.ContainsKey($entry.FullName)){throw 'Duplicate system ZIP member'}
                    $entries[$entry.FullName]=$entry
                }
                if($entries.Count -ne $inventory.Count){throw 'System ZIP file inventory differs from capture'}
                foreach($file in $inventory){
                    $entry=$entries[$file.name]
                    if(-not $entry -or $entry.Length -ne $file.bytes){throw ('Missing/truncated system ZIP member: '+$file.name)}
                    $stream=$entry.Open();$hasher=[Security.Cryptography.SHA256]::Create()
                    try{$entryHash=[BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-','')}
                    finally{$stream.Dispose();$hasher.Dispose()}
                    if($entryHash -ne $file.sha256){throw ('System ZIP SHA256 mismatch: '+$file.name)}
                }
            }finally{$archive.Dispose()}
            # Atomic publication after a full streaming readback. No existing
            # archive is replaced if another publisher won this destination.
            [IO.File]::Move($temp,$destination)
            return [pscustomobject]@{path=$destination;files=$inventory.Count;sha256=(Get-FileHash -LiteralPath $destination).Hash.ToLowerInvariant()}
        }catch{
            $lastError=$_
            if([IO.File]::Exists($destination)){throw 'System ZIP destination already exists; preserving it'}
            if($attempt -lt 3){Start-Sleep -Milliseconds (200*$attempt)}
        }finally{if([IO.File]::Exists($temp)){[IO.File]::Delete($temp)}}
    }
    throw "System ZIP creation/verification failed after 3 attempts: $lastError"
}

function Write-SystemTraceJson {
    param([string]$Path,[object]$Value)
    $temp=$Path+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
    try{
        $Value | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $temp -Encoding UTF8
        if([IO.File]::Exists($Path)){
            [IO.File]::Replace($temp,$Path,[System.Management.Automation.Language.NullString]::Value)
        }else{[IO.File]::Move($temp,$Path)}
    }finally{if([IO.File]::Exists($temp)){[IO.File]::Delete($temp)}}
}
function Get-SystemTraceGameProcesses {
    param([string]$Executable)
    # PID alone can be reused. Identity includes exact path and process birth.
    foreach($process in @(Get-CimInstance Win32_Process -Filter "Name='openmw.exe'")){
        if($process.ExecutablePath -and [string]::Equals([IO.Path]::GetFullPath($process.ExecutablePath),
            $Executable,[StringComparison]::OrdinalIgnoreCase)){
            try{
                # The process can disappear after either lookup. Treat failed
                # native property reads as an exited process, so the recorder
                # loop can save an already observed game instead of cancelling.
                $nativeProcess=Get-Process -Id $process.ProcessId -ErrorAction SilentlyContinue
                if(-not $nativeProcess -or -not [string]::Equals($nativeProcess.Path,$Executable,[StringComparison]::OrdinalIgnoreCase)){continue}
                $creationUtc=$nativeProcess.StartTime.ToUniversalTime().ToString('o')
            }catch{continue}
            [pscustomobject]@{process_id=[int]$process.ProcessId;
                creation_utc=$creationUtc;
                executable_path=[IO.Path]::GetFullPath($process.ExecutablePath)}
        }
    }
}

function Get-SystemTraceStartArguments {
    param([string]$Profile,[string]$Instance,[string]$Mode,[string]$TempDirectory)
    $arguments=@('-start',($Profile+'!OptimizedMW'),'-instancename',$Instance)
    if($Mode -eq 'File'){$arguments+=@('-filemode','-recordtempto',$TempDirectory)}
    return $arguments
}

function Get-SystemTraceStopReason {
    param([double]$ElapsedSeconds,[int]$DurationSeconds,[long]$FreeBytes,[long]$MinimumFreeBytes,
          [bool]$EnterPressed,[bool]$GameObserved,[int]$LiveGameCount)
    if($FreeBytes -lt $MinimumFreeBytes){return 'minimum_free_space'}
    if($ElapsedSeconds -ge $DurationSeconds){return 'duration_limit'}
    if($EnterPressed){return 'user_enter'}
    if($GameObserved -and $LiveGameCount -eq 0){return 'game_exit'}
    return $null
}

function Start-SystemTraceOwnedInstance {
    param([object]$Lease,[string]$Profile,[string]$Mode,[string]$TempDirectory)
    if($Lease.started){throw 'This recorder already owns an active instance'}
    $result=Invoke-SystemRecorder (Get-SystemTraceStartArguments $Profile $Lease.instance $Mode $TempDirectory)
    if($result.Code -eq 0){$Lease.started=$true}
    return $result
}

function Save-SystemTraceOwnedInstance {
    param([object]$Lease,[string]$Etl)
    if(-not $Lease.started){throw 'Refusing to stop an instance this recorder does not own'}
    $result=Invoke-SystemRecorder @('-stop',$Etl,'OptimizedMW full-run hitch investigation','-compress','-skipPdbGen','-instancename',$Lease.instance)
    if($result.Code -eq 0){$Lease.started=$false}
    return $result
}

function Cancel-SystemTraceOwnedInstance {
    param([object]$Lease)
    if(-not $Lease.started){return $null}
    $result=Invoke-SystemRecorder @('-cancel','-instancename',$Lease.instance)
    if($result.Code -eq 0){$Lease.started=$false}
    return $result
}

if($FunctionsOnly){return}
$script:wpr=Join-Path $env:WINDIR 'System32\wpr.exe'
$profile=Join-Path $PSScriptRoot 'OptimizedMW-SystemTrace.wprp'
if(-not (Test-Path -LiteralPath $script:wpr)){throw 'Windows Performance Recorder is unavailable'}
if($ValidateOnly){
    $result=Invoke-SystemRecorder @('-profiles',$profile)
    if($result.Code -ne 0){throw ('WPR rejected the custom profile: '+($result.Lines -join ' '))}
    foreach($variant in @('Memory','File')){
        $arguments=@('-profiledetails',($profile+'!OptimizedMW'))
        if($variant -eq 'File'){$arguments+='-filemode'}
        $result=Invoke-SystemRecorder $arguments
        if($result.Code -ne 0){throw ('WPR could not resolve '+$variant+': '+($result.Lines -join ' '))}
        $result.Lines | Write-Output
    }
    Write-Host 'PASS installed WPR validated both File and Memory profiles; no recording started'
    return
}
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
if(-not ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
    throw 'Right-click START-OptimizedMW-SystemTrace.bat and choose Run as administrator. Start the game from its normal launcher in a separate window.'
}
$exe=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'openmw.exe'))
if(-not (Test-Path -LiteralPath $exe)){throw 'Put this recorder beside the tested openmw.exe'}
if(@(Get-SystemTraceGameProcesses $exe).Count){throw 'Quit the tested game before starting the full-run recorder, then launch it normally after recording starts'}
$minimumFreeBytes=[long]$MinimumFreeGiB*1GB
$freeBytes=Get-SystemTraceFreeBytes $PSScriptRoot
if($freeBytes -lt $minimumFreeBytes){throw "Insufficient free space: recorder requires $MinimumFreeGiB GiB available before starting"}
$activePath=Join-Path $PSScriptRoot 'SYSTEM-TRACE-ACTIVE.json'
if(Test-Path -LiteralPath $activePath){
    $previous=Get-Content -Raw -LiteralPath $activePath | ConvertFrom-Json
    $owner=Get-Process -Id $previous.recorder_process_id -ErrorAction SilentlyContinue
    if($owner -and $owner.StartTime.ToUniversalTime().ToString('o') -eq (ConvertTo-SystemTraceUtcText $previous.recorder_creation_utc)){
        throw 'A system recorder still owns the active-session file; leave its session intact'
    }
}
$captureId=[Guid]::NewGuid().ToString('N')
$directory=Join-Path $PSScriptRoot ('SystemTrace\'+(Get-Date -Format 'yyyyMMdd_HHmmss_fff')+'_'+$captureId.Substring(0,8))
New-Item -ItemType Directory -Path $directory | Out-Null
$tempDirectory=Join-Path $directory 'WPRTemp'
New-Item -ItemType Directory -Path $tempDirectory | Out-Null
$instance='OptimizedMW-'+$captureId
$hash=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()
$state=[ordered]@{schema=2;capture_id=$captureId;instance=$instance;
    recorder_process_id=$PID;recorder_creation_utc=(Get-Process -Id $PID).StartTime.ToUniversalTime().ToString('o');
    directory=$directory;executable_path=$exe;executable_sha256=$hash;game_directory=$PSScriptRoot;
    start_utc=[DateTime]::UtcNow.ToString('o');profile_sha256=(Get-FileHash -LiteralPath $profile).Hash;
    buffer_mode=$Mode;requested_buffer_mib=320;maximum_duration_seconds=$DurationSeconds;
    minimum_free_bytes=$minimumFreeBytes;free_bytes_at_start=$freeBytes;status='starting';
    save_space_guard='Keep minimum free space plus twice current temporary trace bytes for merge/save; checked every second';
    started=$false;normal_stop=$false;events_lost='NOT_YET_ANALYZED';
    completeness='UNQUALIFIED until retained scheduling coverage, loss counts and engine identity are checked';
    gpu_event_availability='NOT_YET_ANALYZED';game_processes=@();
    correlation='Exact executable hash/path, PID plus creation time, capture_id handshake and engine clock-map QPC/UTC anchors';
    limits='Provider configuration does not prove GPU events exist. Private driver internals and absent symbols remain unresolved. File recording introduces disk activity.'}
Copy-Item -LiteralPath $profile -Destination $directory
if(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'CI-ID.txt')){Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'CI-ID.txt') -Destination $directory}
$etl=Join-Path $directory 'OptimizedMW-SystemTrace.etl'
$statePath=Join-Path $directory 'SYSTEM-TRACE-STATE.json'
$lease=[pscustomobject]@{instance=$instance;started=$false};$ownsActive=$false
try{
    # Only this unique instance is ever stopped or cancelled. The file variant
    # preserves early events; duration/space guards bound recording, not buffers.
    $result=Start-SystemTraceOwnedInstance $lease $profile $Mode $tempDirectory
    if($lease.started){$state.started=$true}
    $result.Lines | Set-Content -LiteralPath (Join-Path $directory 'WPR-START.txt')
    if($result.Code -ne 0){throw "WPR start failed ($($result.Code)); the game was not launched by this recorder"}
    $state.recording_started_utc=[DateTime]::UtcNow.ToString('o');$state.status='recording'
    Write-SystemTraceJson $statePath $state
    Write-SystemTraceJson $activePath $state
    $ownsActive=$true
    Write-Host "System trace is recording in $Mode mode; maximum $DurationSeconds seconds." -ForegroundColor Green
    if($Mode -eq 'Memory'){Write-Host 'CIRCULAR CONTROL: early events may be overwritten even with zero reported event loss.' -ForegroundColor Yellow}
    Write-Host 'Use the NORMAL START-OptimizedMW-Test.bat in another window and choose MODE 3.' -ForegroundColor Yellow
    Write-Host 'The recorder saves automatically when that exact game exits. Press Enter here to stop earlier.'
    Write-Host "Recording also stops at the time limit or if free space drops below $MinimumFreeGiB GiB."
    $watch=[Diagnostics.Stopwatch]::StartNew()
    $observed=@{};$gameObserved=$false;$stopReason=$null
    while(-not $stopReason){
        $live=@(Get-SystemTraceGameProcesses $exe)
        if($live.Count -gt 1){throw 'Multiple exact-path games detected; refusing to claim an unambiguous run'}
        foreach($game in $live){
            $key=[string]$game.process_id+'|'+$game.creation_utc
            if(-not $observed.ContainsKey($key)){
                $game | Add-Member observed_utc ([DateTime]::UtcNow.ToString('o'))
                $observed[$key]=$game;$gameObserved=$true
                $state.game_processes=@($observed.Values)
                Write-SystemTraceJson $statePath $state
                Write-SystemTraceJson $activePath $state
                Write-Host ('Associated game PID '+$game.process_id+'; exact executable path verified.') -ForegroundColor Green
            }
        }
        $freeBytes=Get-SystemTraceFreeBytes $directory
        $temporaryBytes=Get-SystemTraceTemporaryBytes $tempDirectory
        $saveReserveBytes=$minimumFreeBytes+2*$temporaryBytes
        $enterPressed=$false
        try{while([Console]::KeyAvailable){if([Console]::ReadKey($true).Key -eq [ConsoleKey]::Enter){$enterPressed=$true}}}catch{
            # Redirected hosts cannot read keys. Exit/time/space guards still work.
        }
        $stopReason=Get-SystemTraceStopReason $watch.Elapsed.TotalSeconds $DurationSeconds $freeBytes $saveReserveBytes $enterPressed $gameObserved $live.Count
        if(-not $stopReason){Start-Sleep -Milliseconds 1000}
    }
    $state.stop_reason=$stopReason;$state.free_bytes_at_stop=$freeBytes
    $state.temporary_trace_bytes_at_stop=$temporaryBytes;$state.save_reserve_bytes_at_stop=$saveReserveBytes
    $state.game_exit_observed_utc=$(if($stopReason -eq 'game_exit'){[DateTime]::UtcNow.ToString('o')}else{$null})
    $state.status='saving';Write-SystemTraceJson $statePath $state;Write-SystemTraceJson $activePath $state
    $result=Invoke-SystemRecorder @('-status','collectors','-details','-instancename',$instance)
    $result.Lines | Set-Content -LiteralPath (Join-Path $directory 'WPR-COLLECTORS-BEFORE-STOP.txt')
    $result=Save-SystemTraceOwnedInstance $lease $etl
    $result.Lines | Set-Content -LiteralPath (Join-Path $directory 'WPR-STOP.txt')
    if($result.Code -ne 0){throw "WPR stop/save failed ($($result.Code)); cleanup cancels only this named instance"}
    if(-not (Test-Path -LiteralPath $etl) -or (Get-Item -LiteralPath $etl).Length -eq 0){throw 'WPR did not produce a nonempty ETL'}
    $state.normal_stop=$true;$state.end_utc=[DateTime]::UtcNow.ToString('o');$state.status='saved'
    $state.etl_sha256=(Get-FileHash -LiteralPath $etl).Hash.ToLowerInvariant();$state.etl_bytes=(Get-Item -LiteralPath $etl).Length
    $state.executable_unchanged=((Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant() -eq $hash)
    Write-SystemTraceJson $statePath $state
    $qualification=Join-Path $PSScriptRoot 'OptimizedMW_QualifySystemTrace.ps1'
    if(Test-Path -LiteralPath $qualification){
        try{& $qualification -CaptureDirectory $directory}catch{
            $state.qualification_error=[string]$_
            Write-Host ('ETL saved; qualification unavailable: '+$_) -ForegroundColor Yellow
        }
    }
    Write-Host ('Saved: '+$etl) -ForegroundColor Green
    Write-Host 'Share this system capture directory and its engine ZIP. The qualification report states actual coverage and provider availability.'
}finally{
    if($lease.started){
        $cleanup=Cancel-SystemTraceOwnedInstance $lease
        $state.cleanup_exit_code=$cleanup.Code
        $cleanup.Lines | Set-Content -LiteralPath (Join-Path $directory 'WPR-CLEANUP.txt')
    }
    if(-not $state.normal_stop){$state.status=$(if($lease.started){'cleanup_failed'}else{'failed_or_cancelled'})}
    Write-SystemTraceJson $statePath $state
    if($ownsActive -and (Test-Path -LiteralPath $activePath)){
        $active=Get-Content -Raw -LiteralPath $activePath | ConvertFrom-Json
        if($active.capture_id -eq $captureId){
            if($lease.started){
                Write-SystemTraceJson $activePath $state
                Write-Warning ('Cleanup could not cancel owned WPR instance '+$instance+'. Its name and temporary files are retained in '+$directory)
            }else{[IO.File]::Delete($activePath)}
        }
    }
}
if($state.normal_stop -and -not $lease.started){
    try{
        $zip=New-VerifiedSystemTraceZip -SourceDirectory $directory -DestinationPath ($directory+'.zip')
        Write-Host ('Verified system ZIP: '+$zip.path) -ForegroundColor Green
        Write-Host 'Share this system ZIP and the matching engine ZIP. If a later .system-qualification.json appears beside the engine ZIP, include it too.'
    }catch{
        Write-Warning ('System ZIP could not be created: '+$_+'. The original ETL and metadata remain at '+$directory)
    }
}
