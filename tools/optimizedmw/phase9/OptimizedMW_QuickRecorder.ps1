param(
    [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-fA-F]{32}$')][string]$SessionId,
    [Parameter(Mandatory=$true)][ValidateRange(1,2147483647)][int]$OwnerProcessId,
    [Parameter(Mandatory=$true)][string]$OwnerCreationUtc,
    [ValidateRange(30,1800)][int]$DurationSeconds=600
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'OptimizedMW_QuickTest.ps1')
$directory=Join-Path $PSScriptRoot ('TestSessions\'+$SessionId)
$resultPath=Join-Path $directory 'RECORDER-RESULT.json'
$result=[ordered]@{schema=1;session_id=$SessionId;status='failed';error=$null;state=$null;system_zip=$null;
    recorder_process_id=$PID;recorder_creation_utc=(Get-Process -Id $PID).StartTime.ToUniversalTime().ToString('o')}
try{
    if(-not [IO.Directory]::Exists($directory)){throw 'The coordinating launcher session directory is missing'}
    $request=Read-OptimizedMWQuickJson (Join-Path $directory 'SESSION-REQUEST.json')
    if($request.schema -ne 1 -or $request.session_id -ne $SessionId -or $request.owner_process_id -ne $OwnerProcessId -or
        (ConvertTo-OptimizedMWQuickUtc $request.owner_creation_utc) -ne (ConvertTo-OptimizedMWQuickUtc $OwnerCreationUtc)){
        throw 'The coordinating launcher request identity differs'
    }
    & (Join-Path $PSScriptRoot 'OptimizedMW_SystemTrace.ps1') -Mode File -DurationSeconds $DurationSeconds `
        -CoordinatorSessionId $SessionId -CoordinatorProcessId $OwnerProcessId -CoordinatorCreationUtc $OwnerCreationUtc
    $statePath=Join-Path $directory 'RECORDER-STATE.json'
    if(-not [IO.File]::Exists($statePath)){throw 'System recorder did not publish its session state'}
    $result.state=Read-OptimizedMWQuickJson $statePath
    $result.system_zip=$result.state.directory+'.zip'
    if($result.state.status -ne 'saved' -or -not $result.state.normal_stop -or -not [IO.File]::Exists($result.system_zip)){
        throw 'Windows trace or its verified ZIP did not finish saving'
    }
    $result.status='complete'
}catch{
    $result.error=[string]$_
    $statePath=Join-Path $directory 'RECORDER-STATE.json'
    if([IO.File]::Exists($statePath)){try{$result.state=Read-OptimizedMWQuickJson $statePath}catch{}}
}finally{
    $result.finished_utc=[DateTime]::UtcNow.ToString('o')
    # This result is published only after the recorder, qualifier and system ZIP
    # path finish. The parent additionally waits for this helper process to exit.
    if([IO.Directory]::Exists($directory)){Write-OptimizedMWQuickJson $resultPath $result -CreateOnly}
}
if($result.status -ne 'complete'){exit 1}
