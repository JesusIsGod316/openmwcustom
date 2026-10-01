$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'OptimizedMW_HitchReport.ps1')
$tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
$profile=Join-Path $tempRoot ('p9-stream-report-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $profile | Out-Null
$path=Join-Path $profile 'p9-hitch-spans.csv'
$header='span,parent,frame,thread,start_ns,end_ns,epoch_ms,phase,identity,asset,wall_ms,children_ms,unattributed_ms,thread_cpu_ms,identity_truncated,link,metric1_name,metric1_value,metric2_name,metric2_value,metric3_name,metric3_value,metric4_name,metric4_value'
$rows=@('2,1,1,1,0,1000000,1000,child,,,1,0,1,-1,0,1,,0,,0,,0,,0',
    '1,0,1,1,0,2000000,1000,engine.iteration,,,2,1,1,-1,0,0,,0,,0,,0,,0',
    '3,0,1,2,0,1000000,1000,worker,,,1,0,1,-1,0,1,,0,,0,,0,,0')
function New-StreamStatus {
    param([UInt64]$Rows=3)
    return [ordered]@{schema=2;transport_schema=1;transport='stream_mpsc_v1';rows=$Rows;capacity=128;dropped=0;allocation_failed=0;active_at_finish=0;normal_finish=1;output_ok=1;
        attempted=$Rows;accepted=$Rows;written=$Rows;pending=0;capacity_dropped=0;contention_dropped=0;allocation_dropped=0;io_dropped=0;stopped_dropped=0;
        initialization_failed=0;writer_failed=0;drain_complete=1;active_producers_at_finish=0;queue_capacity=128;queue_high_water=3;row_size_bytes=864;storage_bytes=110592;writer_thread_id=17;
        writer_started_ns=10;writer_finished_ns=50;writer_cpu_ns=16;writer_cpu_available=1;writer_format_ns=12;writer_write_ns=10;writer_batches=1;writer_serialized_bytes=1024}
}
function Set-StreamFixture {
    param([Collections.IDictionary]$Status,[string[]]$Rows=$rows,[string[]]$ExtraStatus=@(),[string[]]$ExtraFooter=@())
    $lines=@($Status.Keys | ForEach-Object {$_+'='+$Status[$_]})
    [IO.File]::WriteAllLines($path,@($header)+$Rows+@($lines | ForEach-Object {'# '+$_})+$ExtraFooter)
    [IO.File]::WriteAllLines($path+'.capture-status.txt',$lines+$ExtraStatus)
    [IO.File]::WriteAllLines((Join-Path $profile 'TEST_MODE.txt'),@('phase9_hitch_spans=1','phase9_hitch_transport_schema=1','phase9_hitch_transport=stream_mpsc_v1','final_exit_code=0'))
}
function Assert-Rejected {
    param([string]$Name,[Collections.IDictionary]$Status,[string[]]$ExtraStatus=@(),[string[]]$ExtraFooter=@())
    Set-StreamFixture $Status -ExtraStatus $ExtraStatus -ExtraFooter $ExtraFooter
    $result=Test-Phase9HitchCapture -ProfileDir $profile
    if($result.valid_span_capture -or $result.transport_receipt_valid -or -not $result.reasons.Count){throw "Incomplete/contradictory transport accepted: $Name"}
}
function Set-ObserverFixture {
    param([string[]]$Rows,[UInt64]$ThreadSlots=1,[UInt64]$Overflow=0)
    [IO.File]::WriteAllLines($path+'.observer.csv',@('schema,thread_slot,thread,category,calls,wall_ns')+$Rows)
    [IO.File]::WriteAllLines($path+'.observer-status.txt',@('schema=1','thread_capacity=256',"thread_slots=$ThreadSlots","rows=$($Rows.Count)","ledger_overflow=$Overflow",
        'normal_finish=1','output_ok=1','coverage_complete=1','ledger_update_timing=UNMEASURED_count_only_no_recursive_timers',
        'lua_allocator_timer_boundary_timing=UNMEASURED_count_only_existing_clock_calls_excludes_allocator_service',
        'lua_decode_timer_boundary_timing=UNMEASURED_count_only_extra_end_clock_reuses_scope_start',
        'count_only_categories=trace_retained,trace_filtered,ledger_update,lua_allocator_timer_boundary,lua_decode_timer_boundary'))
    [IO.File]::AppendAllText((Join-Path $profile 'TEST_MODE.txt'),"hitch_observer_revision=1`r`n")
}
function New-EventFixtureRow {
    param([UInt64]$Id,[string]$Phase,[UInt64]$Parent=0,[UInt64]$Link=0,[int]$Start=0,[int]$End=3000000,
        [double]$Children=0,[string]$Identity='container',[string]$Asset='event',[Collections.IDictionary]$Metrics=[ordered]@{})
    $culture=[Globalization.CultureInfo]::InvariantCulture;$wall=($End-$Start)/1000000.0
    $fields=@($Id,$Parent,1,1,$Start,$End,1000,$Phase,$Identity,$Asset,$wall.ToString('R',$culture),$Children.ToString('R',$culture),($wall-$Children).ToString('R',$culture),-1,0,$Link)
    foreach($key in $Metrics.Keys){$fields+=@($key,[UInt64]$Metrics[$key])}
    for($slot=$Metrics.Count;$slot -lt 4;$slot++){$fields+=@('',0)}
    return (($fields | ForEach-Object {'"'+([string]$_).Replace('"','""')+'"'}) -join ',')
}
try{
    Set-StreamFixture (New-StreamStatus)
    $good=Test-Phase9HitchCapture -ProfileDir $profile
    if(-not $good.valid_span_capture -or -not $good.transport_receipt_valid -or -not $good.valid_internal_span_joins -or -not $good.report_analysis_complete){throw 'Complete streaming capture rejected'}
    if($good.observer_work.writer.thread_id -ne 17 -or $good.observer_work.writer.cpu_ms -ne .000016 -or $good.observer_work.all_observer_work_measured){throw 'Writer cost ownership/availability changed'}
    foreach($key in @('schema','attempted','written','pending','allocation_dropped','drain_complete','writer_thread_id','writer_started_ns','row_size_bytes')){
        $status=New-StreamStatus;$status.Remove($key);Assert-Rejected "missing $key" $status
    }
    foreach($field in @('capacity_dropped','contention_dropped','allocation_dropped','stopped_dropped')){
        $status=New-StreamStatus;$status[$field]=1;$status.attempted=4;$status.dropped=1;Assert-Rejected $field $status
    }
    $status=New-StreamStatus;$status.accepted=4;$status.attempted=4;$status.pending=1;$status.drain_complete=0;Assert-Rejected 'pending/drain' $status
    $status=New-StreamStatus;$status.attempted=4;$status.accepted=4;$status.io_dropped=1;$status.dropped=1;$status.writer_failed=1;$status.drain_complete=0;Assert-Rejected 'I/O loss' $status
    foreach($key in @('initialization_failed','writer_failed','active_at_finish','allocation_failed')){
        $status=New-StreamStatus;$status[$key]=1;Assert-Rejected $key $status
    }
    $status=New-StreamStatus;$status.written=2;Assert-Rejected 'inconsistent arithmetic' $status
    $status=New-StreamStatus;$status.queue_high_water=129;Assert-Rejected 'queue high water exceeds capacity' $status
    $status=New-StreamStatus;$status.transport_schema=2;Assert-Rejected 'unknown transport schema' $status
    $status=New-StreamStatus;$status.transport='unknown';Assert-Rejected 'unknown backend' $status
    $status=New-StreamStatus;$status['future_unknown_accounting']=0;Assert-Rejected 'unknown accounting field' $status
    $status=New-StreamStatus;$status.Remove('transport_schema');Assert-Rejected 'missing requested schema' $status
    Assert-Rejected 'duplicate status' (New-StreamStatus) -ExtraStatus @('dropped=0')
    Assert-Rejected 'duplicate footer' (New-StreamStatus) -ExtraFooter @('# written=3')
    Assert-Rejected 'unknown footer accounting' (New-StreamStatus) -ExtraFooter @('# unknown_transport_count=0')
    Set-StreamFixture (New-StreamStatus)
    [IO.File]::WriteAllText($path,([IO.File]::ReadAllText($path)).Replace('# writer_thread_id=17','# writer_thread_id=18'))
    $writerContradiction=Test-Phase9HitchCapture $profile
    if($writerContradiction.valid_span_capture -or $writerContradiction.transport_receipt_valid){throw 'Contradictory writer thread receipt/footer accepted'}
    $status=New-StreamStatus;$status.writer_cpu_available=0;Set-StreamFixture $status
    if($null -ne (Test-Phase9HitchCapture $profile).observer_work.writer.cpu_ms){throw 'Unavailable writer CPU was invented as zero'}
    # Transport receipts remain qualified when a semantic dependency is invalid.
    $bad=$rows.Clone();$bad[0]=$bad[0].Replace('2,1,1,1,','2,1,1,2,')
    Set-StreamFixture (New-StreamStatus) -Rows $bad
    $semantic=Test-Phase9HitchCapture $profile
    if(-not $semantic.transport_receipt_valid -or $semantic.valid_internal_span_joins -or $semantic.valid_span_capture){throw 'Semantic joins were conflated with writer completion'}
    if(Test-Path -LiteralPath (Join-Path $profile 'HITCH-PHASE-TOTALS.csv')){throw 'Failed requalification left stale derived totals'}
    # Calibration is preserved independently of transport loss.
    [IO.File]::WriteAllLines($path+'.clock-map.txt',@('process_id=42','thread_id_kind=windows_native_tid','qpc_frequency=10000000',
        'start_qpc_before=1','start_qpc_after=2','start_steady_ns=0','end_qpc_before=100','end_qpc_after=101','end_steady_ns=2000000','normal_finish=1'))
    $status=New-StreamStatus;$status.attempted=4;$status.capacity_dropped=1;$status.dropped=1;Set-StreamFixture $status
    [IO.File]::AppendAllText((Join-Path $profile 'TEST_MODE.txt'),"phase9_detail_revision=4`r`n")
    $lost=Test-Phase9HitchCapture $profile
    if(-not $lost.clock_mapping_valid -or $lost.valid_span_capture -or $lost.clock_mapping.process_id -ne '42'){throw 'Timeline loss hid independent clock calibration'}
    Set-StreamFixture (New-StreamStatus)
    [IO.File]::AppendAllText((Join-Path $profile 'TEST_MODE.txt'),"phase9_detail_revision=4`r`n")
    [IO.File]::WriteAllText($path+'.clock-map.txt',([IO.File]::ReadAllText($path+'.clock-map.txt')).Replace('normal_finish=1','normal_finish=0'))
    $clockBad=Test-Phase9HitchCapture $profile
    if(-not $clockBad.valid_internal_span_joins -or $clockBad.clock_mapping_valid -or $clockBad.valid_span_capture){throw 'Bad clock erased internal joins or qualified ETW mapping'}
    [IO.File]::Delete($path+'.clock-map.txt')
    # Producer coverage is independently qualified, including filtered calls.
    $observerRows=@('1,0,19,trace_scope_entry,4,100','1,0,19,trace_scope_exit,4,200',
        '1,0,19,trace_filtered,3,0','1,0,19,lua_decode_timer_boundary,1,0','1,0,19,ledger_update,12,0')
    Set-StreamFixture (New-StreamStatus);Set-ObserverFixture $observerRows
    $observerGood=Test-Phase9HitchCapture $profile
    if(-not $observerGood.valid_span_capture -or -not $observerGood.observer_work.ledger_valid -or
        ($observerGood.observer_work.ledger.counters | Where-Object category -eq lua_decode_timer_boundary).wall_timing_available){throw 'Complete filtered producer ledger or count-only timing rejected'}
    $badObserver=$observerRows.Clone();$badObserver[3]='1,0,19,lua_decode_timer_boundary,1,7'
    Set-StreamFixture (New-StreamStatus);Set-ObserverFixture $badObserver
    $observerBad=Test-Phase9HitchCapture $profile
    if(-not $observerBad.valid_span_capture -or $observerBad.observer_work.ledger_valid){throw 'Observer timer fiction accepted or changed CPU integrity'}
    Set-StreamFixture (New-StreamStatus);Set-ObserverFixture $observerRows -Overflow 1
    if((Test-Phase9HitchCapture $profile).observer_work.ledger_valid){throw 'Observer ledger overflow accepted'}
    $allCategories=@('trace_scope_entry','trace_scope_exit','trace_append','trace_retained','trace_filtered','catalog_work','catalog_path_collection',
        'catalog_publication','catalog_counter_update','lua_allocator_bookkeeping','lua_allocator_timer_boundary','lua_decode_timer_boundary')
    $maximumLedger=@(for($slot=0;$slot -lt 256;$slot++){
        foreach($category in $allCategories){"1,$slot,$($slot+1),$category,1,0"}
        "1,$slot,$($slot+1),ledger_update,12,0"
    })
    Set-StreamFixture (New-StreamStatus);Set-ObserverFixture $maximumLedger -ThreadSlots 256
    if(-not (Test-Phase9HitchCapture $profile).observer_work.ledger_valid -or $maximumLedger.Count -ne 3328){throw 'Full 13-category fixed ledger rejected'}
    [IO.File]::Delete($path+'.observer.csv');[IO.File]::Delete($path+'.observer-status.txt')
    # Normal capability versions do not activate Deep channels.
    [IO.File]::WriteAllLines((Join-Path $profile 'TEST_MODE.txt'),@('phase9_hitch_spans=0','phase9_detail_revision=4','hitch_diagnostics_revision=2','hitch_observer_revision=1','hitch_lifecycle_status=0','final_exit_code=0'))
    $normal=Test-Phase9HitchCapture $profile;$normalSix=Test-Phase9Diagnostics6Capture $profile
    if($normal.clock_mapping_requested -or $normal.observer_work.ledger_requested -or $normalSix.requested -or $null -ne $normalSix.valid_diagnostics_capture){throw 'Normal capability metadata enabled Deep diagnostics'}
    # Event IDs are exact receipts; fast supplemental shape remains unavailable.
    $eventRows=@(
        (New-EventFixtureRow 100 'lua.event.dispatch' -Children 2 -Metrics ([ordered]@{payload_bytes=24;handler_count=1;decode_outcome=1;invoked_handlers=1}))
        (New-EventFixtureRow 101 'lua.event.deserialize' -Parent 100 -Link 100 -End 1000000 -Children .1 -Metrics ([ordered]@{payload_bytes=24;value_count=3;table_count=1;max_depth=2}))
        (New-EventFixtureRow 102 'lua.event.payload_shape' -Parent 101 -Link 101 -End 100000 -Metrics ([ordered]@{string_count=2;string_bytes=8;custom_values=1;custom_bytes=4}))
        (New-EventFixtureRow 103 'lua.event.handler' -Parent 100 -Link 100 -Start 1000000 -End 2000000 -Identity 'event' -Asset 'script.lua' -Metrics ([ordered]@{script_id=1;reverse_index=0}))
        (New-EventFixtureRow 110 'lua.event.dispatch' -Children 1.1 -Metrics ([ordered]@{payload_bytes=16;handler_count=1;decode_outcome=1;invoked_handlers=1}))
        (New-EventFixtureRow 111 'lua.event.deserialize' -Parent 110 -Link 110 -End 100000 -Metrics ([ordered]@{payload_bytes=16;value_count=2;table_count=0;max_depth=1}))
        (New-EventFixtureRow 112 'lua.event.handler' -Parent 110 -Link 110 -Start 1000000 -End 2000000 -Identity 'event' -Asset 'script.lua' -Metrics ([ordered]@{script_id=1;reverse_index=0}))
        (New-EventFixtureRow 120 'lua.event.dispatch' -Children 1 -Metrics ([ordered]@{payload_bytes=0;handler_count=1;decode_outcome=3;invoked_handlers=0}))
        (New-EventFixtureRow 121 'lua.event.deserialize' -Parent 120 -Link 120 -End 1000000 -Metrics ([ordered]@{payload_bytes=0;value_count=0;table_count=0;max_depth=0}))
        (New-EventFixtureRow 130 'lua.event.dispatch' -Metrics ([ordered]@{payload_bytes=0;decode_outcome=0;invoked_handlers=0}))
    )
    Set-StreamFixture (New-StreamStatus $eventRows.Count) -Rows $eventRows
    $events=Test-Phase9HitchCapture $profile
    if(-not $events.valid_span_capture -or $events.analysis.lua_event_decode.retained_dispatches -ne 4 -or
        $events.analysis.lua_event_decode.slow_shapes -ne 1 -or $events.analysis.lua_event_decode.decoders_without_supplemental_shape -ne 2 -or
        $events.analysis.lua_event_decode.retained_payload_bytes -ne 40 -or $events.analysis.lua_event_decode.retained_string_bytes -ne 8 -or
        $events.analysis.lua_event_decode.empty_nil -ne 1 -or $events.analysis.lua_event_decode.unattempted -ne 1){throw ('Retained Lua event receipt/shape summary changed: '+($events.reasons -join '; '))}
    $badEvents=$eventRows.Clone();$badEvents[2]=$badEvents[2].Replace('"101","string_count"','"100","string_count"')
    Set-StreamFixture (New-StreamStatus $badEvents.Count) -Rows $badEvents
    if((Test-Phase9HitchCapture $profile).valid_span_capture){throw 'Lua payload shape accepted wrong immutable decoder receipt'}
    # More lifetime rows than the old arena maximum; queue capacity stays 128.
    $count=524289;$status=New-StreamStatus $count
    $writer=[IO.StreamWriter]::new($path,$false,[Text.UTF8Encoding]::new($false))
    try{
        $writer.WriteLine($header)
        foreach($row in $rows){$writer.WriteLine($row)}
        for($id=4;$id -le $count;$id++){$writer.WriteLine("$id,0,1,2,0,1000000,1000,volume,,,1,0,1,-1,0,0,,0,,0,,0,,0")}
        foreach($key in $status.Keys){$writer.WriteLine('# '+$key+'='+$status[$key])}
    }finally{$writer.Dispose()}
    [IO.File]::WriteAllLines($path+'.capture-status.txt',@($status.Keys | ForEach-Object {$_+'='+$status[$_]}))
    [IO.File]::WriteAllLines((Join-Path $profile 'TEST_MODE.txt'),@('phase9_hitch_spans=1','phase9_hitch_transport_schema=1','phase9_hitch_transport=stream_mpsc_v1','final_exit_code=0'))
    $long=Test-Phase9HitchCapture $profile
    if(-not $long.valid_span_capture -or $long.analysis.rows -ne $count -or $long.status.rows -le $long.status.capacity){throw 'Streaming lifetime rows incorrectly limited to queue or old arena capacity'}
    # Keep deferred captures independently compatible and capacity bounded.
    Set-StreamFixture (New-StreamStatus)
    $legacy=[ordered]@{rows=3;capacity=131072;dropped=0;allocation_failed=0;active_at_finish=0;normal_finish=1;output_ok=1}
    $lines=@($legacy.Keys | ForEach-Object {$_+'='+$legacy[$_]})
    [IO.File]::WriteAllLines($path,@($header)+$rows+@($lines | ForEach-Object {'# '+$_}))
    [IO.File]::WriteAllLines($path+'.capture-status.txt',$lines)
    [IO.File]::WriteAllLines((Join-Path $profile 'TEST_MODE.txt'),@('phase9_hitch_spans=1','final_exit_code=0'))
    if(-not (Test-Phase9HitchCapture $profile).valid_span_capture){throw 'Legacy deferred capture compatibility changed'}
    Write-Host 'PASS streaming report: exact arbitrary-order parent/causal joins, >524288 lifetime rows, separate receipt/joins/clock qualifications, all producer/writer loss, drain/initialization, contradictory/duplicate/schema status, writer cost availability and legacy deferred control.'
}finally{
    $absolute=[IO.Path]::GetFullPath($profile)
    if(-not $absolute.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Unexpected fixture cleanup path'}
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
