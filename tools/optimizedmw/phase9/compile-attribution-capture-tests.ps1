# Independent synthetic observations; no game/WPR/UI/native DLL execution.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'OptimizedMW_ProfileArchive.ps1')
. (Join-Path $PSScriptRoot 'OptimizedMW_HitchReport.ps1')
$tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
$profile=Join-Path $tempRoot ('p9-compile-capture-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $profile | Out-Null
$timeline=Join-Path $profile 'p9-hitch-spans.csv';$path=Join-Path $profile 'compile-attribution.csv'
$header='schema,frame,epoch_ms,event,context,frame_known,scheduler,set,set_generation,op,span,branch,selected_set_first_seen_frame,selected_set_age_frames,op_first_observed_frame,op_age_observed_frames,demand_first_observed_frame,last_progress_observed_frame,tracking_status,urgency,quarantine,forcing_reason,age_threshold_frames,hard_age_threshold_frames,budget_ms,predicted_ms,actual_ms,target_frame_ms,max_budget_ms,base_target_fps,elapsed_before_ms,last_handoff_ms,headroom_ms,resource_bytes,prepared_reused,required,cancelled,kind,compile_class,texture,image0,image0_revision,image0_filename,image0_known,texture_object_before,texture_object_after,queue_depth,global_oldest_age_frames,active_candidates,quarantined_candidates,metadata_allocation_failures,format_allocation_failures,registry_capacity_failures,registry_allocation_failures'
try{
    @('phase9_hitch_spans=1','hitch_compile_attribution=1','final_exit_code=0') | Set-Content -LiteralPath (Join-Path $profile 'TEST_MODE.txt')
    @('span,parent,frame,thread,start_ns,end_ns,epoch_ms,phase,identity,asset,wall_ms,children_ms,unattributed_ms,thread_cpu_ms,identity_truncated',
        '1,0,7,101,0,10000000,0,compile.scheduler,,,10,2,8,-1,0',
        '2,1,7,101,1000000,3000000,0,compile.operation,,,2,0,2,-1,0',
        '3,0,8,101,11000000,12000000,0,compile.scheduler,,,1,0,1,-1,0',
        '# rows=3','# capacity=131072','# dropped=0','# allocation_failed=0','# active_at_finish=0','# normal_finish=1') |
        Set-Content -LiteralPath $timeline
    @('rows=3','capacity=131072','dropped=0','allocation_failed=0','active_at_finish=0','normal_finish=1','output_ok=1') |
        Set-Content -LiteralPath ($timeline+'.capture-status.txt')
    if(-not (Test-Phase9HitchCapture $profile).valid_span_capture){throw 'Synthetic source timeline invalid'}
    $names=$header.Split(',');$index=@{};for($i=0;$i -lt $names.Count;$i++){$index[$names[$i]]=$i}
    function New-CompileRow {
        param([string]$Event,[int]$Span,[long]$Frame=7)
        [string[]]$fields=@('0')*54
        foreach($name in @('selected_set_first_seen_frame','op_first_observed_frame','op_age_observed_frames','demand_first_observed_frame','last_progress_observed_frame','image0_filename')){$fields[$index[$name]]=''}
        $fields[$index.schema]='1';$fields[$index.frame]=[string]$Frame;$fields[$index.event]=$Event
        $fields[$index.frame_known]=if($Frame -eq 4294967295){'0'}else{'1'}
        $fields[$index.scheduler]='44';$fields[$index.span]=[string]$Span;$fields[$index.branch]='p4'
        $fields[$index.tracking_status]='unobserved';$fields[$index.urgency]='unknown';$fields[$index.quarantine]='unknown';$fields[$index.forcing_reason]='normal'
        $fields[$index.kind]='unknown';$fields[$index.compile_class]='unknown'
        if($Event -eq 'op'){
            $fields[$index.set]='22';$fields[$index.op]='33';$fields[$index.set_generation]='1'
            $fields[$index.selected_set_first_seen_frame]='3';$fields[$index.selected_set_age_frames]='4'
            $fields[$index.op_first_observed_frame]='6';$fields[$index.op_age_observed_frames]='1'
            $fields[$index.demand_first_observed_frame]='7';$fields[$index.tracking_status]='observed';$fields[$index.urgency]='required'
        }
        if($Event -eq 'fallback'){$fields[$index.branch]='osg_fallback'}
        return ,$fields
    }
    [object[]]$rows=@(
        ,(New-CompileRow op 2)
        ,(New-CompileRow summary 1)
        ,(New-CompileRow fallback 3 8)
    )
    function Write-CompileCapture {
        param([object[]]$Rows,[int]$Lost=0,[switch]$NoFooter)
        $lines=[Collections.Generic.List[string]]::new();$lines.Add($header)
        foreach($row in $Rows){$lines.Add((($row | ForEach-Object {'"'+([string]$_).Replace('"','""')+'"'}) -join ','))}
        if(-not $NoFooter){$lines.Add('# p9_capture_transport=bounded_mpsc_v1');$lines.Add("# v3_async_diagnostics_dropped_lines=$Lost")}
        [IO.File]::WriteAllLines($path,$lines)
        @('normal_finish=1','output_ok=1',"rows_dropped=$Lost",'queue_capacity=32768','byte_capacity=33554432','row_capacity=65536',
            "capacity_dropped=$Lost",'contention_dropped=0','oversized_dropped=0','allocation_dropped=0','io_dropped=0','stopped_dropped=0','initialization_failed=0') |
            Set-Content -LiteralPath ($path+'.writer-status.txt')
    }
    Write-CompileCapture $rows
    $result=Test-Phase9CompileAttributionCapture $profile
    if(-not $result.valid_compile_attribution_capture -or $result.analysis.rows -ne 3 -or $result.analysis.operations -ne 1 -or
        $result.analysis.summaries -ne 1 -or $result.analysis.fallbacks -ne 1 -or $result.analysis.unknown_frames -ne 0){throw 'Valid schema1 span/frame/branch joins rejected'}
    foreach($case in @('schema','missing-span','frame','wrong-phase','duplicate','invented-unknown','metadata-failure','format-failure','registry-capacity','registry-allocation','unknown-frame','unknown-capacity','missing-row','lost','footer')){
        [object[]]$bad=@($rows | ForEach-Object {,([string[]]$_.Clone())})
        switch($case){
            'schema'{$bad[0][$index.schema]='2'}
            'missing-span'{$bad[0][$index.span]='0'}
            'frame'{$bad[0][$index.frame]='8'}
            'wrong-phase'{$bad[0][$index.span]='1'}
            'duplicate'{$bad+=,([string[]]$bad[0].Clone())}
            'invented-unknown'{$bad[0][$index.tracking_status]='unknown_allocation'}
            'metadata-failure'{$bad[1][$index.metadata_allocation_failures]='1'}
            'format-failure'{$bad[1][$index.format_allocation_failures]='1'}
            'registry-capacity'{$bad[1][$index.registry_capacity_failures]='1'}
            'registry-allocation'{$bad[1][$index.registry_allocation_failures]='1'}
            'unknown-frame'{$bad[2][$index.frame_known]='0'}
            'unknown-capacity'{$bad[0][$index.tracking_status]='unknown_capacity';$bad[0][$index.set_generation]='0';foreach($name in @('op_first_observed_frame','op_age_observed_frames','demand_first_observed_frame','last_progress_observed_frame')){$bad[0][$index[$name]]=''}}
            'missing-row'{$bad=$bad[0..1]}
        }
        if($case -eq 'lost'){Write-CompileCapture $bad -Lost 1}elseif($case -eq 'footer'){Write-CompileCapture $bad -NoFooter}else{Write-CompileCapture $bad}
        $invalid=Test-Phase9CompileAttributionCapture $profile
        if($invalid.valid_compile_attribution_capture){throw "Invalid compile $case accepted"}
        if(-not (Test-Phase9HitchCapture $profile).valid_span_capture){throw 'Attribution-family failure erased complete source spans'}
    }
    'hitch_compile_attribution=0' | Set-Content -LiteralPath (Join-Path $profile 'TEST_MODE.txt')
    if($null -ne (Test-Phase9CompileAttributionCapture $profile).valid_compile_attribution_capture){throw 'Unrequested compile family promoted'}
    Write-Host 'PASS compile attribution: schema1/54 columns, exact unique retained span/frame/phase links, scheduler/op ownership, unknowns, persistent registry loss, partial tracking, allocation failures, missing rows, transport/loss and independent CPU family validity.'
}finally{
    $absolute=[IO.Path]::GetFullPath($profile)
    if(-not $absolute.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Unexpected fixture cleanup path'}
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
exit 0
