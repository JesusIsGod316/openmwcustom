param([string]$OutputRoot)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'OptimizedMW_ProfileArchive.ps1')
if(-not $OutputRoot){$OutputRoot=Join-Path ([IO.Path]::GetTempPath()) ('optimizedmw-gpu-query-capture-'+[guid]::NewGuid().ToString('N'))}
$null=New-Item -ItemType Directory -Path $OutputRoot
function Require([bool]$Condition,[string]$Message){if(-not $Condition){throw $Message}}
function New-Fixture([string]$Name,[bool]$Pending=$true){
    $dir=Join-Path $OutputRoot $Name;$null=New-Item -ItemType Directory -Path $dir
    $csv=Join-Path $dir 'p9-gpu-passes.csv';$policy='deferred_begin_min2_backoff8'
    @('phase9_gpu_query_accounting=1','phase9_gpu_query_defer=1') | Set-Content -LiteralPath (Join-Path $dir 'TEST_MODE.txt')
    $counts=[ordered]@{issued=3;collected=$(if($Pending){2}else{3});invalid=0;pending=[int]$Pending;active=0;
        admission_dropped=0;unavailable_polls=1;deferred_skips=2;frame_regressions=0;unsupported=0;
        missing_stamp=0;query_allocation_failures=0;nested_begin=0}
    $query=[ordered]@{schema=1;tracker=42;pass='owned long pass label';context=0}
    foreach($key in $counts.Keys){$query[$key]=$counts[$key]}
    [pscustomobject]$query | Export-Csv -LiteralPath ($csv+'.queries.csv') -NoTypeInformation
    $pendingHeader='schema,tracker,pass,context,source_frame,camera,begin_query,end_query,status,next_poll_frame,unavailable_polls'
    if($Pending){@($pendingHeader,'1,42,owned long pass label,0,102,77,5,6,pending,104,0') | Set-Content -LiteralPath ($csv+'.pending.csv')}
    else{$pendingHeader | Set-Content -LiteralPath ($csv+'.pending.csv')}
    $status=[ordered]@{schema=1;normal_finish=1;output_ok=1;producer_threads_joined=1;accounting_complete=1;
        query_result_coverage_complete=[int](-not $Pending);writer_finalization='pending_external_status';policy=$policy;
        registered_trackers=1;registration_failures=0;begin_calls=3}
    foreach($key in $counts.Keys){$status[$key]=$counts[$key]}
    $status.missing_state=0;$status.writer_dropped=0;$status.status_complete=1
    @($status.GetEnumerator() | ForEach-Object {$_.Key+'='+$_.Value}) | Set-Content -LiteralPath ($csv+'.query-status.txt')
    $rows=@('source_frame,report_frame,gpu_ms,context,pass,report_frame_known,collection_policy,writer_dropped_total',
        "100,102,0.1,0,owned long pass label,1,$policy,0","101,103,0.2,0,owned long pass label,1,$policy,0")
    if(-not $Pending){$rows+="102,104,0.3,0,owned long pass label,1,$policy,0"}
    $rows+=@('# p9_capture_transport=bounded_mpsc_v1','# v3_async_diagnostics_dropped_lines=0')
    $rows | Set-Content -LiteralPath $csv
    @('normal_finish=1','output_ok=1','rows_dropped=0','queue_capacity=32768','byte_capacity=33554432','row_capacity=65536',
        'capacity_dropped=0','contention_dropped=0','oversized_dropped=0','allocation_dropped=0','io_dropped=0',
        'stopped_dropped=0','initialization_failed=0') | Set-Content -LiteralPath ($csv+'.writer-status.txt')
    return $dir
}
function Replace-FileText([string]$Path,[string]$Old,[string]$New){
    $text=[IO.File]::ReadAllText($Path);Require ($text.Contains($Old)) ('Fixture token absent: '+$Old)
    [IO.File]::WriteAllText($Path,$text.Replace($Old,$New))
}
$dir=New-Fixture 'honest-pending';$result=Test-Phase9GpuQueryCapture $dir
Require ($result.valid_gpu_accounting -and -not $result.complete_gpu_results) 'Honest EOF pending invalidated accounting or became zero GPU time'
$dir=New-Fixture 'complete' $false;$result=Test-Phase9GpuQueryCapture $dir
Require ($result.valid_gpu_accounting -and $result.complete_gpu_results) 'Complete finalized writer/results rejected'
$mutations=[ordered]@{
    'missing-final-marker'={param($d) Replace-FileText ($d+'\p9-gpu-passes.csv.query-status.txt') 'status_complete=1' 'status_complete=0'}
    'missing-writer-boundary'={param($d) Replace-FileText ($d+'\p9-gpu-passes.csv.query-status.txt') 'writer_finalization=pending_external_status' 'writer_finalization=finished'}
    'wrong-policy'={param($d) Replace-FileText ($d+'\p9-gpu-passes.csv.query-status.txt') 'policy=deferred_begin_min2_backoff8' 'policy=legacy_begin_end'}
    'false-completeness'={param($d) Replace-FileText ($d+'\p9-gpu-passes.csv.query-status.txt') 'query_result_coverage_complete=0' 'query_result_coverage_complete=1'}
    'wrong-issued-count'={param($d) Replace-FileText ($d+'\p9-gpu-passes.csv.query-status.txt') 'issued=3' 'issued=4'}
    'missing-pending-identity'={param($d) $p=$d+'\p9-gpu-passes.csv.pending.csv';[IO.File]::WriteAllText($p,([IO.File]::ReadAllLines($p)[0])+"`n")}
    'wrong-pending-owner'={param($d) Replace-FileText ($d+'\p9-gpu-passes.csv.pending.csv') '1,42,' '1,43,'}
    'duplicate-owner'={param($d) $p=$d+'\p9-gpu-passes.csv.queries.csv';Add-Content -LiteralPath $p ([IO.File]::ReadAllLines($p)[1])}
    'late-writer-failure'={param($d) Replace-FileText ($d+'\p9-gpu-passes.csv.writer-status.txt') 'output_ok=1' 'output_ok=0'}
    'late-writer-loss'={param($d) Replace-FileText ($d+'\p9-gpu-passes.csv.writer-status.txt') 'rows_dropped=0' 'rows_dropped=1';Replace-FileText ($d+'\p9-gpu-passes.csv.writer-status.txt') 'io_dropped=0' 'io_dropped=1'}
    'missing-result-row'={param($d) Replace-FileText ($d+'\p9-gpu-passes.csv') '101,103,0.2,0,owned long pass label,1,deferred_begin_min2_backoff8,0' ''}
    'malformed-result'={param($d) Replace-FileText ($d+'\p9-gpu-passes.csv') '100,102,0.1,0,' '100,102,NaN,0,'}
    'wrong-result-policy'={param($d) Replace-FileText ($d+'\p9-gpu-passes.csv') ',deferred_begin_min2_backoff8,0' ',legacy_begin_end,0'}
}
foreach($name in $mutations.Keys){
    $dir=New-Fixture $name;& $mutations[$name] $dir
    $result=Test-Phase9GpuQueryCapture $dir
    Require ($result.valid_gpu_accounting -eq $false -and $result.complete_gpu_results -eq $false) ('Invalid fixture accepted: '+$name)
}
$dir=New-Fixture 'legacy-not-requested'
'phase9_draw_trace=1' | Set-Content -LiteralPath (Join-Path $dir 'TEST_MODE.txt')
$result=Test-Phase9GpuQueryCapture $dir
Require (-not $result.requested -and $null -eq $result.valid_gpu_accounting) 'Older capture acquired a new mandatory channel'
Write-Host 'PASS GPU query qualification: independent honest pending, finalized writer/row counts, ownership identities, policy and malformed/false claims'
