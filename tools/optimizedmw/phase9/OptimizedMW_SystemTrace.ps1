param([switch]$ValidateOnly)
$ErrorActionPreference='Stop'
$profile=Join-Path $PSScriptRoot 'OptimizedMW-SystemTrace.wprp'
$wpr=Join-Path $env:WINDIR 'System32\wpr.exe'
if(-not (Test-Path -LiteralPath $wpr)){throw 'Windows Performance Recorder is unavailable'}
function Invoke-SystemRecorder {
    param([string[]]$Arguments)
    # PS5 treats native stderr as ErrorRecord objects. Observe the actual exit
    # code even when WPR prints a message there, so a successfully started
    # session always acquires cleanup ownership.
    $ErrorActionPreference='Continue'
    $lines=@(& $wpr @Arguments 2>&1)
    $code=$LASTEXITCODE
    return [pscustomobject]@{Code=$code;Lines=@($lines | ForEach-Object {[string]$_})}
}
if($ValidateOnly){
    & $wpr -profiles $profile
    if($LASTEXITCODE -ne 0){throw 'WPR rejected the custom profile'}
    & $wpr -profiledetails ($profile+'!OptimizedMW')
    if($LASTEXITCODE -ne 0){throw 'WPR could not resolve the custom collectors'}
    Write-Host 'PASS system trace profile validated by installed WPR'
    return
}
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
if(-not ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
    throw 'Right-click START-OptimizedMW-SystemTrace.bat and choose Run as administrator. Start the game from its normal launcher in a separate window.'
}
$exe=Join-Path $PSScriptRoot 'openmw.exe'
if(-not (Test-Path -LiteralPath $exe)){throw 'Put this recorder beside the tested openmw.exe'}
$directory=Join-Path $PSScriptRoot ('SystemTrace\'+(Get-Date -Format 'yyyyMMdd_HHmmss_fff'))
if(Test-Path -LiteralPath $directory){throw 'Existing recording directory must be preserved'}
New-Item -ItemType Directory -Path $directory | Out-Null
$instance='OptimizedMW-'+[Guid]::NewGuid().ToString('N')
$startUtc=[DateTime]::UtcNow.ToString('o')
$hash=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()
$state=[ordered]@{schema=1;instance=$instance;start_utc=$startUtc;executable_sha256=$hash;
    game_directory=$PSScriptRoot;profile_sha256=(Get-FileHash -LiteralPath $profile).Hash;buffer_mode='circular memory';
    requested_buffer_mib=320;started=$false;normal_stop=$false;events_lost='NOT_YET_ANALYZED';
    completeness='UNQUALIFIED until ETL lost-event counts and retained time range are checked';
    correlation='Use p9-hitch-spans.csv.clock-map.txt: process_id, native thread IDs, start/end QPC brackets and steady_ns';
    limits='ETW observes scheduler, IO, faults, sampled stacks and DxgKrnl GPU queues. Private NVIDIA implementation details and absent symbols remain unresolved.'}
Copy-Item -LiteralPath $profile -Destination $directory
if(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'CI-ID.txt')){Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'CI-ID.txt') -Destination $directory}
$etl=Join-Path $directory 'OptimizedMW-SystemTrace.etl'
$started=$false
try{
    # Named instance: never stop/cancel another recorder's session. Memory mode
    # keeps capture buffers bounded and avoids unbounded disk writes while playing.
    $result=Invoke-SystemRecorder @('-start',($profile+'!OptimizedMW'),'-instancename',$instance)
    if($result.Code -eq 0){$started=$true;$state.started=$true}
    $result.Lines | Set-Content -LiteralPath (Join-Path $directory 'WPR-START.txt')
    if($result.Code -ne 0){throw "WPR start failed ($($result.Code)); the game was not launched by this recorder"}
    $started=$true;$state.started=$true
    Write-Host 'System trace is recording with bounded circular buffers.' -ForegroundColor Green
    Write-Host 'Use the NORMAL START-OptimizedMW-Test.bat in another window and choose MODE 3.' -ForegroundColor Yellow
    Write-Host 'After loading, reproduce the hitch for about 30-60 seconds. Quit normally.'
    Write-Host 'Return here immediately after the run. Long recordings can overwrite early events.'
    [void](Read-Host 'Press Enter AFTER quitting the game to stop and save the system trace')
    $result=Invoke-SystemRecorder @('-status','collectors','-details','-instancename',$instance)
    $result.Lines | Set-Content -LiteralPath (Join-Path $directory 'WPR-COLLECTORS-BEFORE-STOP.txt')
    $result=Invoke-SystemRecorder @('-stop',$etl,'OptimizedMW mode 3 hitch investigation','-compress','-skipPdbGen','-instancename',$instance)
    if($result.Code -eq 0){$started=$false}
    $result.Lines | Set-Content -LiteralPath (Join-Path $directory 'WPR-STOP.txt')
    if($result.Code -ne 0){throw "WPR stop/save failed ($($result.Code)). The named instance will be cancelled during cleanup."}
    $started=$false
    if(-not (Test-Path -LiteralPath $etl) -or (Get-Item -LiteralPath $etl).Length -eq 0){throw 'WPR did not produce a nonempty ETL'}
    $state.normal_stop=$true;$state.end_utc=[DateTime]::UtcNow.ToString('o')
    $state.etl_sha256=(Get-FileHash -LiteralPath $etl).Hash.ToLowerInvariant()
    $state.etl_bytes=(Get-Item -LiteralPath $etl).Length
    $state.executable_unchanged=((Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant() -eq $hash)
    $state.recent_captures=@(Get-ChildItem -LiteralPath $PSScriptRoot -File -Filter 'OptimizedMW_Phase9_ROOT-CAUSE-TRACE_*.zip' |
        Where-Object {$_.LastWriteTimeUtc -ge [DateTime]::Parse($startUtc)} |
        ForEach-Object {[pscustomobject]@{name=$_.Name;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}})
    Write-Host ('Saved: '+$etl) -ForegroundColor Green
    Write-Host 'Share this ETL, SYSTEM-TRACE-STATE.json, and the mode 3 ZIP from the same run.'
    Write-Host 'Saving succeeded; event completeness and driver causality still require analysis.'
}finally{
    if($started){
        $cleanup=Invoke-SystemRecorder @('-cancel','-instancename',$instance)
        $state.cleanup_exit_code=$cleanup.Code
        $cleanup.Lines | Set-Content -LiteralPath (Join-Path $directory 'WPR-CLEANUP.txt')
    }
    $state | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $directory 'SYSTEM-TRACE-STATE.json') -Encoding UTF8
}
