param([Parameter(Mandatory=$true)][string]$FixturePrefix)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'OptimizedMW_HitchReport.ps1')
$tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$profile=Join-Path $tempRoot ('p9-hitch-validation-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $profile | Out-Null
try{
    @('phase9_hitch_spans=1','final_exit_code=0') | Set-Content -LiteralPath (Join-Path $profile 'TEST_MODE.txt')
    $target=Join-Path $profile 'p9-hitch-spans.csv'
    foreach($mode in @('normal','overflow')){
        Copy-Item -LiteralPath ($FixturePrefix+'-'+$mode+'.csv') -Destination $target
        Copy-Item -LiteralPath ($FixturePrefix+'-'+$mode+'.csv.capture-status.txt') -Destination ($target+'.capture-status.txt')
        $result=Test-Phase9HitchCapture $profile
        if($result.valid_span_capture -ne ($mode -eq 'normal')){throw "Wrong qualification for $mode"}
        if($mode -eq 'normal' -and $result.analysis.truncated_identities -ne 1){throw 'Truncation was hidden'}
    }
    Copy-Item -LiteralPath ($FixturePrefix+'-normal.csv') -Destination $target
    Copy-Item -LiteralPath ($FixturePrefix+'-normal.csv.capture-status.txt') -Destination ($target+'.capture-status.txt')
    Copy-Item -LiteralPath ($FixturePrefix+'-normal.csv.clock-map.txt') -Destination ($target+'.clock-map.txt')
    @('phase9_hitch_spans=1','phase9_detail_revision=2','final_exit_code=0') | Set-Content -LiteralPath (Join-Path $profile 'TEST_MODE.txt')
    if(-not (Test-Phase9HitchCapture $profile).valid_span_capture){throw 'Clock-calibrated detail capture rejected'}
    $clockOriginal=[IO.File]::ReadAllText($target+'.clock-map.txt')
    [IO.File]::WriteAllText($target+'.clock-map.txt',$clockOriginal.Replace('normal_finish=1','normal_finish=0'))
    if((Test-Phase9HitchCapture $profile).valid_span_capture){throw 'Incomplete clock calibration accepted'}
    [IO.File]::WriteAllText($target+'.clock-map.txt',$clockOriginal)
    @('phase9_hitch_spans=1','phase9_detail_revision=3','final_exit_code=0') | Set-Content -LiteralPath (Join-Path $profile 'TEST_MODE.txt')
    if(-not (Test-Phase9HitchCapture $profile).valid_span_capture){throw 'Revision3 clock-calibrated capture rejected'}
    Remove-Item -LiteralPath ($target+'.clock-map.txt')
    if((Test-Phase9HitchCapture $profile).valid_span_capture){throw 'Revision3 missing clock calibration accepted'}
    [IO.File]::WriteAllText($target+'.clock-map.txt',$clockOriginal)
    @('phase9_hitch_spans=1','final_exit_code=0') | Set-Content -LiteralPath (Join-Path $profile 'TEST_MODE.txt')
    $original=[IO.File]::ReadAllText($target)
    [IO.File]::WriteAllText($target,$original.Replace('# normal_finish=1','# normal_finish=0'))
    if((Test-Phase9HitchCapture $profile).valid_span_capture){throw 'Missing completion accepted'}
    [IO.File]::WriteAllText($target,$original)
    $lines=[IO.File]::ReadAllLines($target)
    for($i=1;$i -lt $lines.Length;$i++){
        if($lines[$i].StartsWith('#') -or -not $lines[$i]){continue}
        $fields=[Phase9.HitchReport]::Csv($lines[$i])
        if($fields[1] -ne '0'){
            $lines[$i]=$lines[$i] -replace '^([0-9]+),[0-9]+,','$1,18446744073709551615,'
            break
        }
    }
    [IO.File]::WriteAllLines($target,$lines)
    if((Test-Phase9HitchCapture $profile).valid_span_capture){throw 'Corrupt parent accepted'}
    [IO.File]::WriteAllText($target,$original)
    Remove-Item -LiteralPath ($target+'.capture-status.txt')
    if((Test-Phase9HitchCapture $profile).valid_span_capture){throw 'Missing independent status accepted'}

    function New-MetadataRow {
        param([UInt64]$Id,[string]$Phase,[UInt64]$Link=0,[System.Collections.IDictionary]$Metrics=@{},[string]$Asset='')
        [string[]]$fields=@([string]$Id,'0','1','1','0','1000000','0',$Phase,'',$Asset,'1','0','1','-1','0',
            [string]$Link,'','0','','0','','0','','0')
        $index=0
        foreach($key in $Metrics.Keys){
            if($index -ge 4){throw 'Fixture metric bound exceeded'}
            $fields[16+$index*2]=[string]$key;$fields[17+$index*2]=[string]$Metrics[$key];$index++
        }
        return $fields
    }
    function Write-MetadataTimeline {
        param([object[]]$Rows,[switch]$OriginalSchema)
        $header='span,parent,frame,thread,start_ns,end_ns,epoch_ms,phase,identity,asset,wall_ms,children_ms,unattributed_ms,thread_cpu_ms,identity_truncated'
        if(-not $OriginalSchema){$header+=',link,metric1_name,metric1_value,metric2_name,metric2_value,metric3_name,metric3_value,metric4_name,metric4_value'}
        $output=[Collections.Generic.List[string]]::new();$output.Add($header)
        foreach($row in $Rows){
            $fields=$row
            if($OriginalSchema){$fields=$row[0..14]}
            $output.Add((($fields | ForEach-Object {'"'+([string]$_).Replace('"','""')+'"'}) -join ','))
        }
        foreach($line in @("rows=$($Rows.Count)",'capacity=131072','dropped=0','allocation_failed=0','active_at_finish=0','normal_finish=1')){$output.Add('# '+$line)}
        [IO.File]::WriteAllLines($target,$output)
        [IO.File]::WriteAllLines($target+'.capture-status.txt',@("rows=$($Rows.Count)",'capacity=131072','dropped=0','allocation_failed=0','active_at_finish=0','normal_finish=1','output_ok=1'))
    }
    function Assert-InvalidMetadata {
        param([object[]]$Rows,[string]$Reason)
        Write-MetadataTimeline -Rows $Rows
        if((Test-Phase9HitchCapture $profile).valid_span_capture){throw "$Reason accepted"}
    }
    [object[]]$metadataRows=@(
        ,(New-MetadataRow 1 'file.open_size' 0 ([ordered]@{region_origin=17;region_size=8192;opened=1}) 'archive,physical.bsa')
        ,(New-MetadataRow 2 'file.stream_totals' 1 ([ordered]@{read_calls=3;returned_bytes=8192;read_call_ns=5000000;seek_calls=1}) 'archive,physical.bsa')
        ,(New-MetadataRow 3 'file.open_size' 0 ([ordered]@{opened=0}) 'missing.nif')
        ,(New-MetadataRow 4 'asset.hash' 0 ([ordered]@{logical_bytes=4096;hash_blocks=1;rewound=1}) 'logical.nif')
        ,(New-MetadataRow 5 'asset.hash' 0 ([ordered]@{logical_bytes=8;hash_blocks=1;rewound=0}) 'failed.nif')
        ,(New-MetadataRow 6 'lua.protected_call' 0 ([ordered]@{alloc_calls=2;alloc_growth_bytes=20;alloc_shrink_bytes=8;alloc_failures=0}))
        ,(New-MetadataRow 7 'diagnostic.resource_catalog')
        ,(New-MetadataRow 8 'file.open_size' 0 ([ordered]@{opened=1}) 'still-open.nif')
    )
    @('phase9_hitch_spans=1','phase9_detail_revision=3','final_exit_code=0') | Set-Content -LiteralPath (Join-Path $profile 'TEST_MODE.txt')
    Write-MetadataTimeline -Rows $metadataRows
    $result=Test-Phase9HitchCapture $profile
    if(-not $result.valid_span_capture){throw '24-column named metrics rejected'}
    if($result.analysis.open_stream_totals_unavailable -ne 1 -or $result.analysis.logical_io.complete_stream_totals){throw 'Missing final stream totals hidden'}
    $io=$result.analysis.logical_io
    if(-not $io.metadata_available -or $io.opened_streams -ne 2 -or $io.failed_opens -ne 1 -or $io.closed_streams -ne 1 -or
        $io.read_calls -ne 3 -or $io.returned_bytes -ne 8192 -or $io.read_call_ns -ne 5000000 -or $io.seek_calls -ne 1 -or
        $io.hash_operations -ne 2 -or $io.hash_logical_bytes -ne 4104 -or $io.hash_blocks -ne 2 -or $io.hash_incomplete_rewinds -ne 1){throw 'Logical I/O aggregates wrong'}
    if($result.analysis.dependency_links -ne 1 -or $result.analysis.diagnostic_phase_totals.Count -ne 1){throw 'Typed dependency/diagnostic summaries wrong'}
    Write-MetadataTimeline -Rows $metadataRows -OriginalSchema
    $result=Test-Phase9HitchCapture $profile
    if(-not $result.valid_span_capture -or $result.analysis.logical_io.metadata_available){throw 'Original15-column compatibility or availability wrong'}
    Write-MetadataTimeline -Rows $metadataRows[0..6]
    $result=Test-Phase9HitchCapture $profile
    if(-not $result.valid_span_capture -or $result.analysis.open_stream_totals_unavailable -ne 0 -or -not $result.analysis.logical_io.complete_stream_totals){throw 'Complete stream totals rejected'}
    foreach($case in @('unnamed','duplicate','too-long','nonnumeric','negative-link','self-link','missing-link','missing-metric','wrong-source','duplicate-totals')){
        [object[]]$bad=@($metadataRows | ForEach-Object { ,([string[]]$_.Clone()) })
        switch($case){
            'unnamed' {$bad[1][16]=''}
            'duplicate' {$bad[1][18]=$bad[1][16]}
            'too-long' {$bad[1][16]='x'*32}
            'nonnumeric' {$bad[1][17]='not-a-number'}
            'negative-link' {$bad[1][15]='-1'}
            'self-link' {$bad[1][15]='2'}
            'missing-link' {$bad[1][0]='200';$bad[1][15]='99'}
            'missing-metric' {$bad[1][16]='';$bad[1][17]='0'}
            'wrong-source' {$bad[0][7]='fixture.other_source'}
            'duplicate-totals' {$copy=[string[]]$bad[1].Clone();$copy[0]='9';$bad+=,$copy}
        }
        Assert-InvalidMetadata $bad $case
    }
    'phase9_hitch_spans=0' | Set-Content -LiteralPath (Join-Path $profile 'TEST_MODE.txt')
    if($null -ne (Test-Phase9HitchCapture $profile).valid_span_capture){throw 'Unrequested trace treated as valid'}
    Write-Host 'PASS hitch qualification: concurrent spans, overflow/truncation, clocks including revision3, original15/new24 schema, logical I/O totals, causal/metric rejection, missing totals/status and unarmed control'
}finally{
    $resolved=[IO.Path]::GetFullPath($profile)
    if(-not $resolved.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Unexpected fixture cleanup path'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
