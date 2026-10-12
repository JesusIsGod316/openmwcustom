param([string]$OutputFile)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'OptimizedMW_MemoryJournal.ps1')
$checks=0
$now=[DateTimeOffset]::UtcNow
function Check($Expected,$Commit,$Physical,$ValidCommit=$true,$ValidPhysical=$true,$Age=0){
    $sample=[pscustomobject]@{EpochMs=$now.ToUnixTimeMilliseconds()-$Age;PerformanceValid=$ValidCommit;PhysicalValid=$ValidPhysical;CommitSpareMiB=$Commit;PhysicalAvailableMiB=$Physical}
    $actual=Get-OptimizedMWMemoryGuardDecision $sample $now
    if($actual -ne $Expected){throw ('Expected '+$Expected+' got '+$actual)}
    $script:checks++
}
Check 'continue' 4096 2048
Check 'continue' 1024 512
Check 'cancel' 1023 2048
Check 'cancel' 4096 511
Check 'cancel' 256 128
Check 'urgent' 255 2048
Check 'urgent' 4096 127
Check 'continue' 0 2048 $false $true
Check 'continue' 4096 0 $true $false
Check 'continue' 0 0 $false $false
Check 'urgent' 0 2048 $true $false
Check 'urgent' 4096 0 $false $true
Check 'unavailable' 0 0 $true $true 2501
Check 'unavailable' 0 0 $true $true -1
Check 'urgent' 0 0 $true $true 2500
if((Get-OptimizedMWMemoryGuardDecision $null $now) -ne 'unavailable'){throw 'Null sample accepted'}
$checks++
# Real harmless children verify cancellation scope. Both are ours; the second
# remains alive until explicit test cleanup, never a game or another user's job.
$owned=$null;$survivor=$null
try{
    $nativePS=Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $owned=Start-Process -FilePath $nativePS -ArgumentList @('-NoProfile','-NonInteractive','-Command','Start-Sleep 30') -WindowStyle Hidden -PassThru
    $survivor=Start-Process -FilePath $nativePS -ArgumentList @('-NoProfile','-NonInteractive','-Command','Start-Sleep 30') -WindowStyle Hidden -PassThru
    Stop-OptimizedMWOwnedAutomaticProcess $owned -Urgent $true
    if(-not $owned.HasExited){throw 'Owned cancellation failed'};$checks++
    if($survivor.HasExited){throw 'Independent helper was stopped'};$checks++
    Stop-OptimizedMWOwnedAutomaticProcess $owned -Urgent $false
    $checks++
    Stop-OptimizedMWOwnedAutomaticProcess $survivor -Urgent $false
    if(-not $survivor.HasExited){throw 'Bounded ordinary cancellation failed'};$checks++
}finally{
    foreach($child in @($owned,$survivor)){if($null -ne $child -and -not $child.HasExited){$child.Kill();$child.WaitForExit()}}
}
$result=[pscustomobject]@{schema=1;passed=$checks;scope='Fresh valid decisions and exact owned-child cancellation; not a memory guarantee'}
if($OutputFile){$result|ConvertTo-Json|Set-Content -LiteralPath $OutputFile -Encoding UTF8}
$result|ConvertTo-Json
