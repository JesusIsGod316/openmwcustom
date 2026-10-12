# Hermetic post-run synthetic timeline validation; no executable fixture needed.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'OptimizedMW_HitchReport.ps1')
. (Join-Path $PSScriptRoot 'OptimizedMW_RunManifest.ps1')
$tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
$profile=Join-Path $tempRoot ('p9-io-capture-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $profile | Out-Null
$path=Join-Path $profile 'p9-hitch-spans.csv'
try{
    function New-IoRow {
        param([UInt64]$Id,[string]$Phase,[UInt64]$Link,[Collections.IDictionary]$Metrics)
        $fields=[Collections.Generic.List[string]]::new()
        foreach($value in @($Id,0,1,1,0,1000000,0,$Phase,'','fixture.bsa',1,0,1,-1,0,$Link)){$fields.Add([string]$value)}
        foreach($key in $Metrics.Keys){$fields.Add([string]$key);$fields.Add([string]$Metrics[$key])}
        while($fields.Count -lt 24){$fields.Add('');$fields.Add('0')}
        if($fields.Count -ne 24){throw 'Fixture row shape'}
        return ,$fields.ToArray()
    }
    function Write-IoCapture {
        param([object[]]$Rows,[int]$Dropped=0)
        $lines=[Collections.Generic.List[string]]::new()
        $lines.Add('span,parent,frame,thread,start_ns,end_ns,epoch_ms,phase,identity,asset,wall_ms,children_ms,unattributed_ms,thread_cpu_ms,identity_truncated,link,metric1_name,metric1_value,metric2_name,metric2_value,metric3_name,metric3_value,metric4_name,metric4_value')
        foreach($row in $Rows){$lines.Add(($row -join ','))}
        $status=@("rows=$($Rows.Count)",'capacity=131072',"dropped=$Dropped",'allocation_failed=0','active_at_finish=0','normal_finish=1')
        foreach($line in $status){$lines.Add('# '+$line)}
        [IO.File]::WriteAllLines($path,$lines)
        [IO.File]::WriteAllLines($path+'.capture-status.txt',@($status)+'output_ok=1')
    }
    [object[]]$rows=@(
        ,(New-IoRow 1 'file.open_size' 0 ([ordered]@{region_origin=17;region_size=8192;opened=1}))
        ,(New-IoRow 2 'file.stream_totals' 1 ([ordered]@{read_calls=3;returned_bytes=8192;read_call_ns=5000000;seek_calls=1}))
        ,(New-IoRow 3 'file.stream_policy' 1 ([ordered]@{requested_refill_bytes=65536;effective_refill_bytes=65536;requested_resident_seek=1;effective_resident_seek=1}))
        ,(New-IoRow 4 'file.stream_buffer' 1 ([ordered]@{owned_candidate_bytes=65536;buffer_cap_hits=0;buffer_allocation_failures=0;budget_limit_bytes=33554432}))
        ,(New-IoRow 5 'file.stream_io' 1 ([ordered]@{native_tell_calls=1;native_seek_calls=2;zero_byte_read_calls=1;resident_seek_hits=3}))
        ,(New-IoRow 6 'file.stream_consumption' 1 ([ordered]@{net_consumed_bytes=9000;net_putback_bytes=100;discarded_unread_bytes=1000;unread_at_close_bytes=0}))
        ,(New-IoRow 7 'file.stream_budget' 1 ([ordered]@{budget_in_use_bytes=0;budget_peak_bytes=66000;active_candidate_streams=0;active_candidate_buffers=0}))
    )
    @('phase9_hitch_spans=1','final_exit_code=0') | Set-Content -LiteralPath (Join-Path $profile 'TEST_MODE.txt')
    Write-IoCapture $rows
    $result=Test-Phase9HitchCapture $profile
    $io=$result.analysis.logical_io
    if(-not $result.valid_span_capture -or -not $io.complete_stream_totals -or -not $io.complete_candidate_stream_totals -or
        $io.policy_streams -ne 1 -or $io.refill_effective_streams -ne 1 -or $io.resident_seek_effective_streams -ne 1 -or
        $io.native_seek_calls -ne 2 -or $io.resident_seek_hits -ne 3 -or $io.net_consumed_bytes -ne 9000 -or
        $io.owned_candidate_bytes_at_close_sum -ne 65536){throw 'Extended linked I/O data or accounting semantics rejected'}
    foreach($length in @(2,3,4,5,6)){
        Write-IoCapture $rows[0..($length-1)]
        $result=Test-Phase9HitchCapture $profile
        if(-not $result.valid_span_capture -or $result.analysis.logical_io.complete_candidate_stream_totals){throw 'Missing extended family hidden or unrelated spans discarded'}
    }
    [object[]]$fallback=@($rows | ForEach-Object {,([string[]]$_.Clone())})
    $fallback[2][19]='8192';$fallback[2][23]='0';$fallback[3][17]='0';$fallback[3][19]='1'
    Write-IoCapture $fallback
    $result=Test-Phase9HitchCapture $profile
    if(-not $result.valid_span_capture -or $result.analysis.logical_io.refill_fallback_streams -ne 1 -or
        $result.analysis.logical_io.resident_seek_fallback_streams -ne 1 -or $result.analysis.logical_io.buffer_cap_hits -ne 1){throw 'Candidate fallback hidden'}
    foreach($case in @('bad-link','bad-asset','duplicate','missing-metric','invalid-refill','unrequested-seek','above-budget','above-max-budget','snapshot-above-budget','unrequested-budget','overflow')){
        [object[]]$bad=@($rows | ForEach-Object {,([string[]]$_.Clone())})
        switch($case){
            'bad-link'{$bad[2][15]='2'}
            'bad-asset'{$bad[2][9]='different.bsa'}
            'duplicate'{$copy=[string[]]$bad[2].Clone();$copy[0]='8';$bad+=,$copy}
            'missing-metric'{$bad[2][16]='';$bad[2][17]='0'}
            'invalid-refill'{$bad[2][19]='262144'}
            'unrequested-seek'{$bad[2][21]='0'}
            'above-budget'{$bad[3][23]='32768'}
            'above-max-budget'{$bad[3][23]='33554433'}
            'snapshot-above-budget'{$bad[6][19]='33554433'}
            'unrequested-budget'{$bad[2][17]='8192';$bad[2][19]='8192';$bad[2][21]='0';$bad[2][23]='0'}
            'overflow'{$bad[5][17]='18446744073709551616'}
        }
        Write-IoCapture $bad
        if((Test-Phase9HitchCapture $profile).valid_span_capture){throw "Malformed extended $case accepted"}
    }
    Write-IoCapture $rows -Dropped 1
    if((Test-Phase9HitchCapture $profile).valid_span_capture){throw 'Extended family weakened loss rejection'}
    Write-IoCapture $rows
    $null=Test-Phase9HitchCapture $profile
    $manifest=[pscustomobject]@{candidate=(Get-Phase9Candidate I1I2);qualifications=[ordered]@{};activation_evidence=[ordered]@{state='pending'}}
    Complete-Phase9RunManifest $manifest $profile
    if(-not $manifest.activation_evidence.io.selection_consistent -or -not $manifest.activation_evidence.io.complete_candidate_stream_totals){throw 'Runtime policy not joined into manifest'}
    $manifest.candidate=Get-Phase9Candidate CONTROL
    Complete-Phase9RunManifest $manifest $profile
    if($manifest.activation_evidence.io.selection_consistent){throw 'Requested/effective policy mismatch hidden'}
    Write-Host 'PASS extended I/O capture: linked unique policy/buffer/native/consumption rows, partial family truth, effective fallback, policy/budget/identity/metric corruption, unchanged explicit loss checks and manifest activation evidence.'
}finally{
    $absolute=[IO.Path]::GetFullPath($profile)
    if(-not $absolute.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Unexpected fixture cleanup target'}
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
exit 0
