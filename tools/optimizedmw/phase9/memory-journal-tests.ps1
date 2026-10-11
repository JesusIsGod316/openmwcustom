param([string]$OutputDirectory)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'OptimizedMW_MemoryJournal.ps1')
if (-not $OutputDirectory) {
    $OutputDirectory=Join-Path ([IO.Path]::GetTempPath()) ('optimizedmw-memory-journal-tests-'+[Guid]::NewGuid().ToString('N'))
}
if ([IO.Directory]::Exists($OutputDirectory) -or [IO.File]::Exists($OutputDirectory)) { throw 'Tests require a fresh output directory.' }
$null=[IO.Directory]::CreateDirectory($OutputDirectory)
$checks=0
function Assert-Journal([bool]$Condition,[string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}
function Read-JournalLive([string]$Path) {
    $stream=[IO.FileStream]::new($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    $reader=[IO.StreamReader]::new($stream)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}
$process=[Diagnostics.Process]::GetCurrentProcess()
$start=[DateTimeOffset]::UtcNow
$cultureBefore=[Threading.Thread]::CurrentThread.CurrentCulture
$journals=New-Object 'System.Collections.Generic.List[object]'
try {
    $missing=Join-Path $OutputDirectory 'not-created'
    $refused=$false
    try { $null=Start-OptimizedMWMemoryJournal -Directory $missing } catch { $refused=$true }
    Assert-Journal $refused 'Missing directory was not refused.'
    Assert-Journal (-not [IO.Directory]::Exists($missing)) 'Start created an unrequested directory.'
    $refused=$false
    try { $null=Start-OptimizedMWMemoryJournal -Directory '' } catch { $refused=$true }
    Assert-Journal $refused 'Empty directory was not refused.'

    $normal=Join-Path $OutputDirectory 'normal'; $null=[IO.Directory]::CreateDirectory($normal)
    $journal=Start-OptimizedMWMemoryJournal -Directory $normal -IncludeSystem; $journals.Add($journal)
    Assert-Journal ($journal.MaxSamples -eq 8192) 'Production cap is not 8192.'
    [Threading.Thread]::CurrentThread.CurrentCulture=[Globalization.CultureInfo]::GetCultureInfo('de-DE')
    Add-OptimizedMWMemorySample -Journal $journal -Process $process -Now $start -Start $start
    Add-OptimizedMWMemorySample -Journal $journal -Process $process -Now $start.AddSeconds(1) -Start $start
    $text=Read-JournalLive $journal.ProcessPath
    $lines=@($text.TrimEnd() -split '\r?\n')
    Assert-Journal ($lines.Count -eq 3) 'Process rows were not flushed before Stop.'
    Assert-Journal ($lines[0] -eq 'epoch_ms,elapsed_s,working_set_mb,private_mb,virtual_mb,cpu_total_s') 'Process schema changed.'
    foreach ($line in $lines[1..2]) {
        $values=$line.Split(',')
        Assert-Journal ($values.Count -eq 6) 'Locale changed column count.'
        foreach ($value in $values) { Assert-Journal ($value -match '^\d+(\.\d+)?$') 'Non-invariant numeric value.' }
    }
    $system=@((Read-JournalLive $journal.SystemPath).TrimEnd() -split '\r?\n')
    Assert-Journal ($system.Count -eq 3) 'System rows were not flushed before Stop.'
    $systemRow=$system[1].Split(',')
    Assert-Journal ($systemRow.Count -eq 12) 'System schema changed.'
    Assert-Journal ($systemRow[2] -eq '1' -and $systemRow[3] -eq '1' -and $systemRow[4] -eq '1') 'Actual current-process Windows sample is invalid.'
    Assert-Journal ([double]::Parse($systemRow[6],[Globalization.CultureInfo]::InvariantCulture) -gt 0) 'Commit limit is absent.'
    Stop-OptimizedMWMemoryJournal $journal -NormalFinish $true
    $status=Get-Content -LiteralPath $journal.StatusPath -Raw | ConvertFrom-Json
    Assert-Journal ($status.complete -and $status.output_ok -and $status.diagnostic_only) 'Successful completion metadata is invalid.'
    Assert-Journal $status.normal_finish 'Explicit successful game result was not retained.'
    Assert-Journal ($status.process_rows -eq 2 -and $status.system_rows -eq 2 -and $status.errors -eq 0) 'Completion row counts are invalid.'
    $refused=$false
    try { $null=Start-OptimizedMWMemoryJournal -Directory $normal } catch { $refused=$true }
    Assert-Journal $refused 'Existing journal was overwritten.'
    $refused=$false
    try { Add-OptimizedMWMemorySample -Journal $journal -Process $process -Now $start.AddSeconds(2) -Start $start } catch { $refused=$true }
    Assert-Journal $refused 'Stopped journal accepted a sample.'

    $failure=Join-Path $OutputDirectory 'invalid-process'; $null=[IO.Directory]::CreateDirectory($failure)
    $journal=Start-OptimizedMWMemoryJournal -Directory $failure -IncludeSystem; $journals.Add($journal)
    $unbound=[Diagnostics.Process]::new()
    try { Add-OptimizedMWMemorySample -Journal $journal -Process $unbound -Now $start -Start $start } finally { $unbound.Dispose() }
    Assert-Journal ($journal.ProcessRows -eq 0 -and $journal.ProcessSampleErrors -eq 1) 'Failed process sample was not recorded.'
    $system=@((Read-JournalLive $journal.SystemPath).TrimEnd() -split '\r?\n')
    Assert-Journal ($system.Count -eq 2 -and $system[1].Split(',')[4] -eq '0') 'Invalid process destroyed the system observation.'
    Stop-OptimizedMWMemoryJournal $journal
    $status=Get-Content -LiteralPath $journal.StatusPath -Raw | ConvertFrom-Json
    Assert-Journal (-not $status.complete -and $status.output_ok -and $status.errors -eq 1) 'Sample failure was reported as valid completion.'
    Assert-Journal (-not $status.normal_finish) 'Default Stop falsely certified game completion.'

    $cap=Join-Path $OutputDirectory 'cap'; $null=[IO.Directory]::CreateDirectory($cap)
    $journal=Start-OptimizedMWMemoryJournal -Directory $cap; $journals.Add($journal)
    $journal.MaxSamples=2
    for($i=0;$i -lt 4;$i++) { Add-OptimizedMWMemorySample -Journal $journal -Process $process -Now $start.AddSeconds($i) -Start $start }
    Assert-Journal ($journal.Attempts -eq 4 -and $journal.AcceptedAttempts -eq 2 -and $journal.Dropped -eq 2) 'Cap did not bound independent attempts.'
    Assert-Journal (@((Read-JournalLive $journal.ProcessPath).TrimEnd() -split '\r?\n').Count -eq 3) 'Cap did not preserve exactly the streamed rows.'
    Assert-Journal (-not [IO.File]::Exists($journal.SystemPath)) 'Opt-in system file was produced by default.'
    Stop-OptimizedMWMemoryJournal $journal
    $status=Get-Content -LiteralPath $journal.StatusPath -Raw | ConvertFrom-Json
    Assert-Journal (-not $status.complete -and $status.output_ok -and $status.dropped_samples -eq 2 -and $status.process_rows -eq 2) 'Cap drop/status parity is invalid.'
    Write-Output ('MEMORY_JOURNAL_PASS checks='+$checks+' shell='+$PSVersionTable.PSVersion.ToString()+' output='+$OutputDirectory)
}
finally {
    [Threading.Thread]::CurrentThread.CurrentCulture=$cultureBefore
    foreach($journal in $journals) { if (-not $journal.Stopped) { try { Stop-OptimizedMWMemoryJournal $journal } catch {} } }
    $process.Dispose()
}
