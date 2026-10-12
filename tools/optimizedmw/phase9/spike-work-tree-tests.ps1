$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'OptimizedMW_SpikeReport.ps1')
$tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
$profile=Join-Path $tempRoot ('p9-spike-work-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $profile | Out-Null
function New-NodeRow {
    param([UInt64]$Id,[UInt64]$Parent,[int]$Frame,[UInt64]$Thread,[long]$Start,[long]$End,[string]$Phase,[UInt64]$Link=0,[double]$Children=0)
    $wall=($End-$Start)/1000000.0
    $fields=@($Id,$Parent,$Frame,$Thread,$Start,$End,(1000+$Frame*10),$Phase,'fixture','model, "quoted".nif',
        $wall.ToString('R',[Globalization.CultureInfo]::InvariantCulture),$Children.ToString('R',[Globalization.CultureInfo]::InvariantCulture),
        ($wall-$Children).ToString('R',[Globalization.CultureInfo]::InvariantCulture),-1,0,$Link,'',0,'',0,'',0,'',0)
    return (($fields | ForEach-Object {'"'+([string]$_).Replace('"','""')+'"'}) -join ',')
}
function Set-Nodes {
    param([string[]]$Rows)
    $header='span,parent,frame,thread,start_ns,end_ns,epoch_ms,phase,identity,asset,wall_ms,children_ms,unattributed_ms,thread_cpu_ms,identity_truncated,link,metric1_name,metric1_value,metric2_name,metric2_value,metric3_name,metric3_value,metric4_name,metric4_value'
    $status=@("rows=$($Rows.Count)",'capacity=131072','dropped=0','allocation_failed=0','active_at_finish=0','normal_finish=1')
    [IO.File]::WriteAllLines((Join-Path $profile 'p9-hitch-spans.csv'),@($header)+$Rows+@($status | ForEach-Object {'# '+$_}))
    [IO.File]::WriteAllLines((Join-Path $profile 'p9-hitch-spans.csv.capture-status.txt'),$status+@('output_ok=1'))
    $null=Test-Phase9HitchCapture -ProfileDir $profile
}
try{
    [IO.File]::WriteAllLines((Join-Path $profile 'TEST_MODE.txt'),@('phase9_hitch_spans=1','final_exit_code=0'))
    $frames=@('frame,epoch_ms,wall_ms,rendering_traversal_ms,accounted_ms,other_ms')
    $frames+=@(1..82 | ForEach-Object {"$_,$(1000+$_*10),$(if($_ -eq 62){20}else{10}),0,0,0"})
    [IO.File]::WriteAllLines((Join-Path $profile 'v3-frame.csv'),$frames)
    $rows=@(
        (New-NodeRow 1 0 1 1 1000000 2000000 'queue.enqueue')
        (New-NodeRow 2 0 62 1 100000000 120000000 'engine.iteration' 0 12)
        (New-NodeRow 3 2 62 1 101000000 105000000 'draw.work')
        (New-NodeRow 4 2 62 1 106000000 110000000 'lua.ui.measure')
        (New-NodeRow 5 0 1 2 103000000 115000000 'queue.execute' 1)
        (New-NodeRow 6 0 62 2 150000000 156000000 'future.worker' 1)
        (New-NodeRow 7 0 63 1 119000000 121000000 'crossing.scope')
        (New-NodeRow 8 0 62 1 100000000 120000000 'frame.wall_interval')
    )
    Set-Nodes $rows
    & (Join-Path $PSScriptRoot 'OptimizedMW_Benchmark_Report.ps1') -ProfileDir $profile
    $summary=Get-Content -Raw -LiteralPath (Join-Path $profile 'HITCH-SPIKE-REPORT.json') | ConvertFrom-Json
    $trees=@(Get-Content -LiteralPath (Join-Path $profile 'HITCH-SPIKE-WORK-TREES.jsonl') | ForEach-Object {$_ | ConvertFrom-Json})
    if(-not $summary.complete -or $summary.analysis.exact_anchors -ne 1 -or $trees.Count -ne 1){throw '20ms relative-spike exact tree was not produced'}
    $tree=$trees[0]
    if($tree.frame -ne 62 -or $tree.anchor_kind -ne 'exact_engine_wall_interval' -or -not $tree.relative_spike -or $tree.absolute_slow){throw 'Below-threshold relative frame was misclassified'}
    if(5 -notin $tree.overlapping_worker_span_ids -or 6 -in $tree.overlapping_worker_span_ids -or 1 -notin $tree.causal_context_span_ids){throw 'Worker origin frame was mistaken for execution overlap or lost enqueue context'}
    if(7 -in $tree.largest_contained_unattributed_span_ids -or $tree.main_thread_envelope_covered_ms -ne 20 -or $tree.outside_retained_main_envelopes_ms -ne 0){throw 'Cross-boundary self time or same-thread coverage was invented'}
    $nodes=Import-Csv -LiteralPath (Join-Path $profile 'HITCH-SPIKE-NODES.csv')
    if(@($nodes).Count -ne 7 -or @($nodes | Where-Object {$_.span -eq '6'}).Count -or $nodes[0].asset -ne 'model, "quoted".nif'){throw 'Node table identity/filter changed'}
    $capture=Get-Content -Raw -LiteralPath (Join-Path $profile 'HITCH-CAPTURE.json') | ConvertFrom-Json
    if('frame.wall_interval' -in $capture.analysis.phase_totals.phase){throw 'Frame anchor was charged as work'}
    $pacingPath=Join-Path $profile 'Phase9-FRAME-PACING.csv'
    $pacing=[IO.File]::ReadAllText($pacingPath)
    [IO.File]::WriteAllText($pacingPath,$pacing.Replace('62,1620,','62,2620,'))
    if((Write-Phase9SpikeWorkReport -ProfileDir $profile).complete){throw 'Stale pacing epoch accepted'}
    [IO.File]::WriteAllText($pacingPath,$pacing)
    # A saved successful qualification must not bless a replaced status file.
    [IO.File]::WriteAllText((Join-Path $profile 'p9-hitch-spans.csv.capture-status.txt'),'normal_finish=0')
    if((Write-Phase9SpikeWorkReport -ProfileDir $profile).complete){throw 'Stale successful capture qualification accepted'}
    Set-Nodes $rows
    $null=Write-Phase9SpikeWorkReport -ProfileDir $profile
    [IO.File]::Delete((Join-Path $profile 'p9-hitch-spans.csv'))
    if((Write-Phase9SpikeWorkReport -ProfileDir $profile).complete -or
        (Test-Path -LiteralPath (Join-Path $profile 'HITCH-SPIKE-NODES.csv'))){throw 'Missing raw source left stale success nodes'}
    Set-Nodes $rows[0..6]
    $partial=Write-Phase9SpikeWorkReport -ProfileDir $profile
    if($partial.complete -or $partial.analysis.iteration_fallbacks -ne 1){throw 'Missing exact anchor falsely claimed complete frame coverage'}
    Set-Nodes ($rows+@(New-NodeRow 9 0 62 1 100000000 120000000 'frame.wall_interval'))
    if((Write-Phase9SpikeWorkReport -ProfileDir $profile).complete){throw 'Duplicate exact anchor accepted'}
    if(Test-Path -LiteralPath (Join-Path $profile 'HITCH-SPIKE-WORK-TREES.jsonl')){throw 'Failed join left a stale successful work tree'}
    $bad=$rows.Clone()
    $bad[0]=New-NodeRow 1 0 1 2 0 200000000 'other.worker'
    $bad[2]=New-NodeRow 3 1 62 1 101000000 105000000 'draw.work'
    Set-Nodes $bad
    if((Write-Phase9SpikeWorkReport -ProfileDir $profile).complete){throw 'Invalid cross-thread nesting accepted'}
    Write-Host 'PASS spike work trees: subthreshold relative spike, exact frame anchor, immutable IDs, nested versus causal/worker overlap, future-worker exclusion, interval union, crossing self-time limits, CSV quoting and fail-closed replacement.'
}finally{
    $absolute=[IO.Path]::GetFullPath($profile)
    if(-not $absolute.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Unexpected fixture cleanup path'}
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
