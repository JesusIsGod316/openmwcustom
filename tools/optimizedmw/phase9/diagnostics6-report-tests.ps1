$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'OptimizedMW_HitchReport.ps1')
$tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
$profile=Join-Path $tempRoot ('p9-diagnostics6-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $profile | Out-Null
$culture=[Globalization.CultureInfo]::InvariantCulture

function New-DiagnosticNode {
    param([UInt64]$Id,[string]$Phase,[UInt64]$Parent=0,[UInt64]$Link=0,[int]$Frame=62,
        [UInt64]$Thread=1,[long]$Start=100000000,[long]$End=110000000,[double]$Children=0,
        [string]$Identity='fixture',[string]$Asset='model, "quoted".nif',
        [Collections.IDictionary]$Metrics=[ordered]@{})
    if($Metrics.Count -gt 4){throw 'Fixture exceeds production metric slots'}
    $wall=($End-$Start)/1000000.0
    $fields=@($Id,$Parent,$Frame,$Thread,$Start,$End,1620,$Phase,$Identity,$Asset,
        $wall.ToString('R',$culture),$Children.ToString('R',$culture),
        ($wall-$Children).ToString('R',$culture),-1,0,$Link)
    foreach($name in $Metrics.Keys){$fields+=@($name,[UInt64]$Metrics[$name])}
    for($slot=$Metrics.Count;$slot -lt 4;$slot++){$fields+=@('',0)}
    return (($fields | ForEach-Object {'"'+([string]$_).Replace('"','""')+'"'}) -join ',')
}

function New-ValidDiagnosticRows {
    param([UInt64]$CapacityFailures=0,[UInt64]$AllocationFailures=0,[UInt64]$MetadataTruncations=0,
        [UInt64]$UnknownOrigins=0,[UInt64]$UnknownGenerations=0,[UInt64]$UnknownUses=0,
        [UInt64]$PublicationFlags=2,[UInt64]$DemandFlags=18,[UInt64]$DemandLink=2,
        [UInt64]$AutomaticGcObserved=0,[UInt64]$TrackingAvailable=1,
        [string]$DemandAsset='model, "quoted".nif')
    @(
        (New-DiagnosticNode 1 'resource.preparation_origin' -Frame 61 -Thread 2 -Start 10000000 -End 100000000 -Children 20)
        (New-DiagnosticNode 2 'resource.lifecycle_publish' -Parent 1 -Link $(if(($PublicationFlags -band 3) -eq 2){1}else{0}) -Frame 61 -Thread 2 -Start 20000000 -End 30000000 -Identity 'template;cache=abc' -Metrics ([ordered]@{lifecycle_id=1;vfs_generation=7;resource_ptr=4096;source_flags=$PublicationFlags}))
        (New-DiagnosticNode 3 'resource.lifecycle_prepare_hit' -Parent 1 -Link 1 -Frame 61 -Thread 2 -Start 40000000 -End 50000000 -Identity 'template;cache=abc' -Metrics ([ordered]@{lifecycle_id=1;resource_ptr=4096;vfs_generation=7;publication_span=2}))
        (New-DiagnosticNode 4 'resource.lifecycle_first_demand' -Link $DemandLink -Start 150000000 -End 160000000 -Identity 'template;cache=abc' -Asset $DemandAsset -Metrics ([ordered]@{lifecycle_id=1;resource_ptr=4096;cache_hit=1;source_flags=$DemandFlags}))
        (New-DiagnosticNode 5 'resource.lifecycle_retire' -Link 2 -Start 180000000 -End 190000000 -Identity 'template;cache=abc' -Metrics ([ordered]@{lifecycle_id=1;reason=1;remaining_refs=1;retained_estimate=0}))
        (New-DiagnosticNode 6 'resource.lifecycle_status' -Start 200000000 -End 230000000 -Children 10 -Metrics ([ordered]@{publications=1;capacity_failures=$CapacityFailures;allocation_failures=$AllocationFailures;metadata_truncations=$MetadataTruncations}))
        (New-DiagnosticNode 7 'resource.lifecycle_coverage' -Parent 6 -Start 210000000 -End 220000000 -Metrics ([ordered]@{active=0;unknown_origins=$UnknownOrigins;unknown_generations=$UnknownGenerations;unknown_uses=$UnknownUses}))
        (New-DiagnosticNode 8 'lua.gc.observation' -Start 300000000 -End 310000000 -Identity 'unavailable_without_vm_sampler' -Asset 'LuaJIT 2.1' -Metrics ([ordered]@{automatic_gc_observed=$AutomaticGcObserved;tracking_available=$TrackingAvailable;instruction_profiler=0;vm_sampler_enabled=0}))
        (New-DiagnosticNode 9 'physics.cycle.enqueue' -Frame 60 -Start 320000000 -End 325000000 -Metrics ([ordered]@{simulations=1;steps=1;workers=2}))
        (New-DiagnosticNode 10 'physics.cycle.worker' -Link 9 -Frame 60 -Thread 3 -Start 330000000 -End 390000000 -Metrics ([ordered]@{queue_delay_ns=5000000;simulations=1;workers=2}))
        (New-DiagnosticNode 11 'navigation.job.enqueue' -Frame 60 -Thread 2 -Start 400000000 -End 410000000 -Metrics ([ordered]@{job_id=7;change_type=1}))
        (New-DiagnosticNode 12 'navigation.job.complete' -Link 11 -Frame 60 -Thread 2 -Start 440000000 -End 450000000 -Metrics ([ordered]@{job_id=7;job_origin=11;outcome=0;job_state=0}))
        (New-DiagnosticNode 13 'workqueue.enqueue' -Frame 61 -Start 460000000 -End 470000000 -Metrics ([ordered]@{queue_depth=1;front=0;item_address=4096}))
        (New-DiagnosticNode 14 'workqueue.execute' -Link 13 -Frame 61 -Thread 4 -Start 480000000 -End 500000000)
        (New-DiagnosticNode 15 'resource.template_bounds' -Start 510000000 -End 520000000)
        (New-DiagnosticNode 16 'lua.alloc.service' -Start 530000000 -End 540000000 -Metrics ([ordered]@{service_ns=10000;service_calls=1;service_failures=0;policy_blocked=0}))
        (New-DiagnosticNode 17 'lua.gc.step_metrics' -Start 550000000 -End 560000000 -Identity 'explicit_step' -Asset '1' -Metrics ([ordered]@{requested_steps=1;cycle_completed=0;heap_before_bytes=1024;heap_after_bytes=1024}))
    )
}

function Set-DiagnosticCapture {
    param([string[]]$Rows,[bool]$Requested=$true,[UInt64]$Dropped=0)
    [IO.File]::Delete((Join-Path $profile 'p9-hitch-spans.csv.lifecycle-status.txt'))
    $header='span,parent,frame,thread,start_ns,end_ns,epoch_ms,phase,identity,asset,wall_ms,children_ms,unattributed_ms,thread_cpu_ms,identity_truncated,link,metric1_name,metric1_value,metric2_name,metric2_value,metric3_name,metric3_value,metric4_name,metric4_value'
    $status=@("rows=$($Rows.Count)",'capacity=131072',"dropped=$Dropped",'allocation_failed=0','active_at_finish=0','normal_finish=1')
    [IO.File]::WriteAllLines((Join-Path $profile 'p9-hitch-spans.csv'),@($header)+$Rows+@($status | ForEach-Object {'# '+$_}))
    [IO.File]::WriteAllLines((Join-Path $profile 'p9-hitch-spans.csv.capture-status.txt'),$status+@('output_ok=1'))
    [IO.File]::WriteAllLines((Join-Path $profile 'TEST_MODE.txt'),@('phase9_hitch_spans=1','final_exit_code=0')+$(if($Requested){@('hitch_diagnostics_revision=1')}else{@()}))
    return (Test-Phase9HitchCapture -ProfileDir $profile)
}

function Set-IndependentLifecycleReceipt {
    param([UInt64]$StatusId,[UInt64]$CoverageId,[UInt64]$Publications=1,[UInt64]$Active=0)
    $receipt=[ordered]@{schema=1;registry_capacity=65536;status_span_id=$StatusId;coverage_span_id=$CoverageId;
        publications=$Publications;active=$Active;first_demands=1;prepare_hits=1;retirements=1;
        capacity_failures=0;allocation_failures=0;metadata_truncations=0;unknown_origins=0;unknown_generations=0;unknown_uses=0;normal_finish=1;output_ok=1}
    [IO.File]::WriteAllLines((Join-Path $profile 'p9-hitch-spans.csv.lifecycle-status.txt'),@($receipt.Keys | ForEach-Object {$_+'='+$receipt[$_]}))
    [IO.File]::AppendAllText((Join-Path $profile 'TEST_MODE.txt'),"hitch_diagnostics_revision=2`r`nhitch_lifecycle_status=1`r`n")
}

function New-InvalidatedDiagnosticRows {
    param([UInt64]$InvalidationLink=2,[bool]$DemandAfter=$false)
    # A valid preparation and (optionally) first retrieval can precede source
    # invalidation. Later retrievals cannot establish reuse of that generation.
    $remove=if($DemandAfter){'^"(4|5|6|7)",'}else{'^"(5|6|7)",'}
    $rows=@(New-ValidDiagnosticRows | Where-Object {$_ -notmatch $remove})
    $rows+=@(
        (New-DiagnosticNode 18 'resource.lifecycle_invalidation' -Link $InvalidationLink -Start 600000000 -End 610000000 -Identity 'template;cache=abc' -Metrics ([ordered]@{lifecycle_id=1;observed_generation=8;reason_flags=4;source_flags=22}))
        (New-DiagnosticNode 19 'resource.lifecycle_status' -Start 700000000 -End 730000000 -Children 10 -Metrics ([ordered]@{publications=1;capacity_failures=0;allocation_failures=0;metadata_truncations=0}))
        (New-DiagnosticNode 20 'resource.lifecycle_coverage' -Parent 19 -Start 710000000 -End 720000000 -Metrics ([ordered]@{active=1;unknown_origins=0;unknown_generations=1;unknown_uses=1}))
    )
    if($DemandAfter){
        $rows+=@(New-DiagnosticNode 21 'resource.lifecycle_first_demand' -Link 2 -Start 650000000 -End 660000000 -Identity 'template;cache=abc' -Metrics ([ordered]@{lifecycle_id=1;resource_ptr=4096;cache_hit=1;source_flags=22}))
    }
    return $rows
}

function Invoke-DiagnosticCase {
    param([string]$Name,[string[]]$Rows,[bool]$Valid,[bool]$Requested=$true,[bool]$CpuValid=$true,[UInt64]$Dropped=0)
    $cpu=Set-DiagnosticCapture -Rows $Rows -Requested $Requested -Dropped $Dropped
    if($cpu.valid_span_capture -ne $CpuValid){throw "$Name CPU qualification unexpected: $($cpu.reasons -join '; ')"}
    $report=Test-Phase9Diagnostics6Capture -ProfileDir $profile
    if($report.requested -ne $Requested){throw "$Name changed explicit revision activation"}
    if($Requested){
        if($report.valid_diagnostics_capture -ne $Valid){throw "$Name family qualification unexpected: $($report.reasons -join '; ')"}
        if(-not $Valid -and $report.reasons.Count -eq 0){throw "$Name failed without an explicit reason"}
    }elseif($null -ne $report.valid_diagnostics_capture -or $null -ne $report.analysis){throw "$Name invented legacy metadata requirements"}
    $cpuAfter=Get-Content -Raw -LiteralPath (Join-Path $profile 'HITCH-CAPTURE.json') | ConvertFrom-Json
    if($cpuAfter.valid_span_capture -ne $CpuValid){throw "$Name auxiliary provenance changed CPU timeline qualification"}
    if($report.all_causes_observed -ne $false -or $report.automatic_gc_observation -ne 'unavailable_without_vm_sampler'){
        throw "$Name invented automatic VM GC or all-cause coverage"
    }
    return $report
}

try{
    $legacy=@(New-DiagnosticNode 1 'engine.iteration')
    $null=Invoke-DiagnosticCase -Name 'legacy absence' -Rows $legacy -Requested $false -Valid $true
    $null=Invoke-DiagnosticCase -Name 'new missing status' -Rows $legacy -Valid $false
    $rows=New-ValidDiagnosticRows
    $good=Invoke-DiagnosticCase -Name 'exact lifecycle and real phase names' -Rows $rows -Valid $true
    if($good.analysis.publications -ne 1 -or $good.analysis.first_retrievals -ne 1 -or
        $good.analysis.preparation_hits -ne 1 -or $good.analysis.retirements -ne 1 -or
        $good.analysis.lua_vms -ne 1 -or -not $good.analysis.origin_generation_provenance_complete){throw 'Exact lifecycle joins lost valid receipts'}
    $expected=[ordered]@{preparation_to_first_api_retrieval=3;model_construction=1;physics_navigation=4;queues_and_resource_retirement=3;lua_allocator_and_explicit_gc=3}
    foreach($group in $expected.Keys){
        if($good.analysis.retained_group_rows[$group] -ne $expected[$group]){throw "Wrong actual phase grouping: $group"}
    }
    foreach($failure in @('CapacityFailures','AllocationFailures','MetadataTruncations')){
        $parameters=@{};$parameters[$failure]=1
        $lost=Invoke-DiagnosticCase -Name $failure -Rows (New-ValidDiagnosticRows @parameters) -Valid $false
        if($null -eq $lost.analysis -or $lost.analysis.lifecycle_metadata_complete){throw "$failure loss did not preserve explicit partial analysis"}
    }
    $unknownRows=@(New-ValidDiagnosticRows -PublicationFlags 4 -DemandFlags 20 -UnknownOrigins 1 -UnknownGenerations 1 -UnknownUses 2 | Where-Object {$_ -notmatch '^"(3|4)",'})
    $unknown=Invoke-DiagnosticCase -Name 'unknown provenance' -Rows $unknownRows -Valid $true
    if(-not $unknown.analysis.lifecycle_metadata_complete -or $unknown.analysis.origin_generation_provenance_complete -or
        $unknown.analysis.unknown_origins -ne 1 -or $unknown.analysis.unknown_generations -ne 1 -or $unknown.analysis.unknown_uses -ne 2){throw 'Unknown provenance was conflated with lost metadata or complete origins'}
    $null=Invoke-DiagnosticCase -Name 'wrong lifetime link' -Rows (New-ValidDiagnosticRows -DemandLink 1) -Valid $false
    $null=Invoke-DiagnosticCase -Name 'wrong lifetime asset' -Rows (New-ValidDiagnosticRows -DemandAsset 'different.nif') -Valid $false
    $null=Invoke-DiagnosticCase -Name 'hidden unknown origin' -Rows (New-ValidDiagnosticRows -PublicationFlags 0 -DemandFlags 16) -Valid $false
    $null=Invoke-DiagnosticCase -Name 'hidden unknown generation' -Rows (New-ValidDiagnosticRows -PublicationFlags 6 -DemandFlags 22) -Valid $false
    $null=Invoke-DiagnosticCase -Name 'changed immutable origin' -Rows (New-ValidDiagnosticRows -DemandFlags 17) -Valid $false
    $invalidated=Invoke-DiagnosticCase -Name 'source invalidated after valid retrieval' -Rows (New-InvalidatedDiagnosticRows) -Valid $true
    if($invalidated.analysis.invalidations -ne 1 -or $invalidated.analysis.first_retrievals -ne 1 -or
        $invalidated.analysis.unknown_generations -ne 1 -or $invalidated.analysis.origin_generation_provenance_complete){throw 'Source invalidation erased prior valid retrieval or invented current-generation reuse'}
    $null=Invoke-DiagnosticCase -Name 'wrong invalidation lifetime link' -Rows (New-InvalidatedDiagnosticRows -InvalidationLink 1) -Valid $false
    $null=Invoke-DiagnosticCase -Name 'first retrieval after invalidation' -Rows (New-InvalidatedDiagnosticRows -DemandAfter $true) -Valid $false
    $null=Invoke-DiagnosticCase -Name 'invented automatic GC coverage' -Rows (New-ValidDiagnosticRows -AutomaticGcObserved 1) -Valid $false
    $tracking=Invoke-DiagnosticCase -Name 'tracking allocator unavailable' -Rows (New-ValidDiagnosticRows -TrackingAvailable 0) -Valid $true
    if($tracking.analysis.lua_vms -ne 1){throw 'Missing allocator tracking suppressed honest VM coverage receipt'}
    $null=Invoke-DiagnosticCase -Name 'global timeline loss' -Rows $rows -Valid $false -CpuValid $false -Dropped 1
    $zero=@(
        (New-DiagnosticNode 1 'resource.lifecycle_status' -Children 2 -Metrics ([ordered]@{publications=0;capacity_failures=0;allocation_failures=0;metadata_truncations=0}))
        (New-DiagnosticNode 2 'resource.lifecycle_coverage' -Parent 1 -Start 102000000 -End 104000000 -Metrics ([ordered]@{active=0;unknown_origins=0;unknown_generations=0;unknown_uses=0}))
    )
    $empty=Invoke-DiagnosticCase -Name 'explicit zero-resource status' -Rows $zero -Valid $true
    if($empty.analysis.publications -ne 0 -or $empty.analysis.lua_vms -ne 0){throw 'Zero resources implied nonexistent VM or resource work'}
    # Exact uint64 receipts must survive beyond floating-point integer precision.
    [UInt64]$largeStatus=9007199254740993;[UInt64]$largeCoverage=9007199254740994
    $largeRows=@(New-ValidDiagnosticRows | ForEach-Object {
        if($_ -match '^"6",'){$_ -replace '^"6",',('"'+$largeStatus+'",')}
        elseif($_ -match '^"7","6",'){$_ -replace '^"7","6",',('"'+$largeCoverage+'","'+$largeStatus+'",')}
        else{$_}
    })
    $null=Set-DiagnosticCapture -Rows $largeRows
    Set-IndependentLifecycleReceipt $largeStatus $largeCoverage
    $receiptGood=Test-Phase9Diagnostics6Capture $profile
    if(-not $receiptGood.valid_diagnostics_capture -or -not $receiptGood.independent_lifecycle_status_valid -or
        $receiptGood.analysis.status_span_id -ne $largeStatus -or $receiptGood.independent_lifecycle_status.coverage_span_id -ne $largeCoverage){throw 'Independent lifecycle receipts lost exact uint64 IDs'}
    Set-IndependentLifecycleReceipt $largeStatus $largeCoverage -Publications 2 -Active 1
    $contradiction=Test-Phase9Diagnostics6Capture $profile
    if($contradiction.valid_diagnostics_capture -or -not $contradiction.independent_lifecycle_status_valid -or
        $contradiction.reasons -notmatch 'disagrees'){throw 'Contradictory retained/independent lifecycle counters qualified exact joins'}
    [IO.File]::Delete((Join-Path $profile 'p9-hitch-spans.csv.lifecycle-status.txt'))
    $missing=Test-Phase9Diagnostics6Capture $profile
    if($missing.valid_diagnostics_capture -or $missing.independent_lifecycle_status_valid){throw 'Requested missing independent lifecycle sidecar accepted'}
    $null=Set-DiagnosticCapture -Rows $largeRows -Dropped 1
    Set-IndependentLifecycleReceipt $largeStatus $largeCoverage
    $independent=Test-Phase9Diagnostics6Capture $profile
    if($independent.valid_diagnostics_capture -or -not $independent.independent_lifecycle_status_valid -or $null -ne $independent.analysis){throw 'Independent registry snapshot invented missing lifetime joins or was hidden by CPU loss'}
    Write-Host 'PASS diagnostics6 qualification: legacy activation, mandatory new status, exact lifetime joins, independent provenance/capacity loss, actual group phase names, honest Lua coverage and unchanged CPU validity.'
}finally{
    $absolute=[IO.Path]::GetFullPath($profile)
    if(-not $absolute.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Unexpected fixture cleanup path'}
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
