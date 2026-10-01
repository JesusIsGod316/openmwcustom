# Library used by the normal launcher. Linking is optional and failures never
# prevent gameplay or alter the final engine profile/archive.
function ConvertTo-Phase9SystemTraceUtcString {
    param([object]$Value)
    if($Value -is [DateTime]){return $Value.ToUniversalTime().ToString('o')}
    return [DateTime]::Parse([string]$Value).ToUniversalTime().ToString('o')
}

function Write-Phase9SystemTraceLinkJson {
    param([string]$Path,[object]$Value)
    $temp=$Path+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
    try{
        $Value | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $temp -Encoding UTF8
        if([IO.File]::Exists($Path)){
            [IO.File]::Replace($temp,$Path,[System.Management.Automation.Language.NullString]::Value)
        }else{[IO.File]::Move($temp,$Path)}
    }finally{if([IO.File]::Exists($temp)){[IO.File]::Delete($temp)}}
}

function Test-Phase9SystemTraceLinkDirectory {
    param([string]$GameDir,[string]$Directory)
    $root=[IO.Path]::GetFullPath((Join-Path $GameDir 'SystemTrace')).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
    $target=[IO.Path]::GetFullPath($Directory).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
    return $target.StartsWith($root,[StringComparison]::OrdinalIgnoreCase) -and $target -ne $root
}

function New-Phase9SystemTraceLink {
    param([Parameter(Mandatory=$true)][string]$GameDir,
          [Parameter(Mandatory=$true)][string]$ProfileDir,
          [Parameter(Mandatory=$true)][string]$Executable,
          [Parameter(Mandatory=$true)][string]$ExecutableSha256)
    try{
        $game=[IO.Path]::GetFullPath($GameDir)
        $activePath=Join-Path $game 'SYSTEM-TRACE-ACTIVE.json'
        if(-not [IO.File]::Exists($activePath)){return $null}
        $active=Get-Content -Raw -LiteralPath $activePath -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop
        $exe=[IO.Path]::GetFullPath($Executable)
        if($active.schema -ne 2 -or $active.status -ne 'recording' -or -not $active.started -or $active.normal_stop -or
            $active.capture_id -notmatch '^[0-9a-fA-F]{32}$' -or $active.instance -ne ('OptimizedMW-'+$active.capture_id)){
            throw 'Active recorder metadata is stale or is not recording'
        }
        if(-not [string]::Equals($exe,[IO.Path]::GetFullPath((Join-Path $game 'openmw.exe')),[StringComparison]::OrdinalIgnoreCase) -or
            -not [string]::Equals($exe,[IO.Path]::GetFullPath($active.executable_path),[StringComparison]::OrdinalIgnoreCase) -or
            -not [string]::Equals($game,[IO.Path]::GetFullPath($active.game_directory),[StringComparison]::OrdinalIgnoreCase) -or
            $ExecutableSha256 -notmatch '^[0-9a-fA-F]{64}$' -or $active.executable_sha256 -ne $ExecutableSha256 -or
            (Get-FileHash -LiteralPath $exe -ErrorAction Stop).Hash -ne $ExecutableSha256){
            throw 'Active recorder executable path/hash does not match the game being launched'
        }
        $recorder=Get-Process -Id ([int]$active.recorder_process_id) -ErrorAction SilentlyContinue
        if(-not $recorder -or $recorder.StartTime.ToUniversalTime().ToString('o') -ne (ConvertTo-Phase9SystemTraceUtcString $active.recorder_creation_utc)){
            throw 'Active recorder PID no longer has the recorded native creation time'
        }
        if(-not (Test-Phase9SystemTraceLinkDirectory $game $active.directory) -or -not [IO.Directory]::Exists($active.directory)){
            throw 'Active recorder directory is outside this game SystemTrace directory or is missing'
        }
        # The recorder writes atomically, so preserve the complete snapshot.
        # Re-read before linking to catch a session that stopped during preflight.
        $latest=Get-Content -Raw -LiteralPath $activePath -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop
        if($latest.capture_id -ne $active.capture_id -or $latest.status -ne 'recording' -or
            $latest.recorder_process_id -ne $active.recorder_process_id -or
            (ConvertTo-Phase9SystemTraceUtcString $latest.recorder_creation_utc) -ne (ConvertTo-Phase9SystemTraceUtcString $active.recorder_creation_utc)){
            throw 'Recorder changed or stopped during launcher preflight'
        }
        $link=[pscustomobject][ordered]@{schema=1;capture_id=$active.capture_id;instance=$active.instance;
            recorder_process_id=[int]$active.recorder_process_id;recorder_creation_utc=(ConvertTo-Phase9SystemTraceUtcString $active.recorder_creation_utc);
            directory=[IO.Path]::GetFullPath($active.directory);game_directory=$game;
            executable_path=$exe;executable_sha256=$ExecutableSha256.ToLowerInvariant();
            recording_started_utc=(ConvertTo-Phase9SystemTraceUtcString $active.recording_started_utc);linked_utc=[DateTime]::UtcNow.ToString('o')}
        Write-Phase9SystemTraceLinkJson (Join-Path $ProfileDir 'SYSTEM-TRACE-LINK.json') $link
        Write-Host ('Linked Windows system capture '+$link.capture_id+'; exact executable and recorder lifetime verified.') -ForegroundColor Green
        return $link
    }catch{
        Write-Warning -WarningAction Continue ('Windows system trace was not linked: '+$_+'. The normal game run can continue.')
        return $null
    }
}

function Invoke-Phase9SystemTraceQualification {
    param([string]$GameDir,[string]$CaptureDirectory,[string]$ProfileZip)
    $helper=Join-Path $GameDir 'OptimizedMW_QualifySystemTrace.ps1'
    if(-not [IO.File]::Exists($helper)){throw 'System trace qualification helper is unavailable; final binding is retained'}
    & $helper -CaptureDirectory $CaptureDirectory -ProfileZip $ProfileZip
}

function Complete-Phase9SystemTraceLink {
    param([object]$Link,
          [Parameter(Mandatory=$true)][string]$ProfileZip,
          [Parameter(Mandatory=$true)][int]$GameProcessId,
          [Parameter(Mandatory=$true)][string]$GameCreationUtc)
    if($null -eq $Link){return $null}
    $binding=$null;$published=$false
    try{
        if($Link.schema -ne 1 -or $Link.capture_id -notmatch '^[0-9a-fA-F]{32}$' -or
            $Link.instance -ne ('OptimizedMW-'+$Link.capture_id) -or $GameProcessId -le 0 -or
            -not (Test-Phase9SystemTraceLinkDirectory $Link.game_directory $Link.directory)){
            throw 'System trace link or game process identity is invalid'
        }
        $birth=[DateTime]::Parse($GameCreationUtc).ToUniversalTime().ToString('o')
        $zipPath=[IO.Path]::GetFullPath($ProfileZip)
        $zipInfo=Get-Item -LiteralPath $zipPath -ErrorAction Stop
        if($zipInfo.Length -le 0){throw 'Final profile ZIP is empty'}
        # Verify that the final ZIP itself carries this handshake. Filename and
        # timestamp similarity alone never pair a system trace with an engine run.
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive=[IO.Compression.ZipFile]::OpenRead($zipPath)
        try{
            $entries=@($archive.Entries | Where-Object {$_.FullName.Replace('\','/') -eq 'SYSTEM-TRACE-LINK.json'})
            if($entries.Count -ne 1 -or $entries[0].Length -gt 1MB){throw 'Final ZIP has no unique bounded system trace link'}
            $reader=[IO.StreamReader]::new($entries[0].Open())
            try{$archivedLink=$reader.ReadToEnd() | ConvertFrom-Json -ErrorAction Stop}finally{$reader.Dispose()}
            foreach($field in @('capture_id','instance','recorder_process_id','recorder_creation_utc','executable_path','executable_sha256','directory')){
                $archived=$archivedLink.$field;$linked=$Link.$field
                if($field -eq 'recorder_creation_utc'){$archived=ConvertTo-Phase9SystemTraceUtcString $archived;$linked=ConvertTo-Phase9SystemTraceUtcString $linked}
                if($archived -ne $linked){throw ('Final ZIP system trace link differs: '+$field)}
            }
        }finally{$archive.Dispose()}
        $statePath=Join-Path $Link.directory 'SYSTEM-TRACE-STATE.json'
        $state=Get-Content -Raw -LiteralPath $statePath -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop
        if($state.schema -ne 2 -or $state.capture_id -ne $Link.capture_id -or $state.instance -ne $Link.instance -or
            $state.recorder_process_id -ne $Link.recorder_process_id -or
            (ConvertTo-Phase9SystemTraceUtcString $state.recorder_creation_utc) -ne (ConvertTo-Phase9SystemTraceUtcString $Link.recorder_creation_utc)){
            throw 'System recorder state belongs to another lifetime'
        }
        $binding=[pscustomobject][ordered]@{schema=1;capture_id=$Link.capture_id;instance=$Link.instance;
            recorder_process_id=[int]$Link.recorder_process_id;recorder_creation_utc=$Link.recorder_creation_utc;
            engine_zip_path=$zipPath;engine_zip_sha256=(Get-FileHash -LiteralPath $zipPath -ErrorAction Stop).Hash.ToLowerInvariant();
            engine_zip_bytes=$zipInfo.Length;game_process_id=$GameProcessId;game_creation_utc=$birth;
            finalized_utc=[DateTime]::UtcNow.ToString('o')}
        $bindingPath=Join-Path $Link.directory 'SYSTEM-TRACE-ENGINE-BINDING.json'
        if([IO.File]::Exists($bindingPath)){
            $existing=Get-Content -Raw -LiteralPath $bindingPath -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop
            foreach($field in @('capture_id','instance','recorder_process_id','recorder_creation_utc','engine_zip_path','engine_zip_sha256','engine_zip_bytes','game_process_id','game_creation_utc')){
                $prior=$existing.$field;$next=$binding.$field
                if($field -in @('recorder_creation_utc','game_creation_utc')){$prior=ConvertTo-Phase9SystemTraceUtcString $prior;$next=ConvertTo-Phase9SystemTraceUtcString $next}
                if($prior -ne $next){throw ('Refusing to replace a different final engine binding: '+$field)}
            }
        }else{Write-Phase9SystemTraceLinkJson $bindingPath $binding}
        $published=$true
        # Qualifier returns promptly with PENDING_OR_UNAVAILABLE while WPR is
        # merging. Its saved-state invocation consumes this binding automatically.
        Invoke-Phase9SystemTraceQualification $Link.game_directory $Link.directory $zipPath
        if($published){return $binding}
        return $null
    }catch{
        Write-Warning -WarningAction Continue ('Windows system trace binding/qualification did not finish: '+$_+'. Final engine ZIP is unchanged.')
        if($published){return $binding}
        return $null
    }
}
