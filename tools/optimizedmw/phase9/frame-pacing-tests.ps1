$ErrorActionPreference='Stop'
$tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$profile=Join-Path $tempRoot ('p9-frame-pacing-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $profile | Out-Null
function Measure-Fixture {
    param([double[]]$Walls,[int[]]$Frames)
    $rows=[Collections.Generic.List[string]]::new()
    [void]$rows.Add('frame,epoch_ms,wall_ms,rendering_traversal_ms,accounted_ms,other_ms')
    for($i=0;$i -lt $Walls.Count;$i++){
        $frame=if($Frames){$Frames[$i]}else{$i+1}
        [void]$rows.Add(('{0},{1},{2},0,0,0' -f $frame,(1000+$i*10),$Walls[$i].ToString('R',[Globalization.CultureInfo]::InvariantCulture)))
    }
    [IO.File]::WriteAllLines((Join-Path $profile 'v3-frame.csv'),$rows)
    & (Join-Path $PSScriptRoot 'OptimizedMW_Benchmark_Report.ps1') -ProfileDir $profile
    return (Get-Content -Raw -LiteralPath (Join-Path $profile 'Phase9-BENCHMARK-REPORT.json') | ConvertFrom-Json)
}
try{
    $smooth=Measure-Fixture (@(10)*240)
    $alternating=Measure-Fixture (@(8,12)*120)
    if($smooth.unfiltered.mean_ms -ne $alternating.unfiltered.mean_ms -or $smooth.frame_pacing.rms_change_ms -ne 0){throw 'Constant pacing baseline changed'}
    if($alternating.unfiltered.population_stddev_ms -ne 2 -or $alternating.frame_pacing.rms_change_ms -ne 4 -or
       $alternating.frame_pacing.mean_absolute_change_ms -ne 4 -or $alternating.unfiltered.gt33_333_frames -ne 0 -or
       @($alternating.contiguous_120_frame_windows).Count -ne 2){throw 'Subthreshold alternating pacing was missed'}
    $spike=Measure-Fixture (@(10)*61+@(20)+@(10)*20)
    $csv=Import-Csv -LiteralPath (Join-Path $profile 'Phase9-FRAME-PACING.csv')
    if($spike.frame_pacing.relative_spike_frames -ne 1 -or $csv[61].baseline_ms -ne '10' -or
       $csv[61].residual_ms -ne '10' -or $csv[61].relative_spike -ne '1' -or @($csv).Count -ne 82){throw '20ms local spike on 10ms baseline was missed'}
    $gaps=Measure-Fixture @(10,10,20,20) @(1,2,10,11)
    if($gaps.frame_discontinuities -ne 1 -or $gaps.frame_pacing.contiguous_pairs -ne 2 -or
       $gaps.frame_pacing.max_absolute_change_ms -ne 0 -or @($gaps.contiguous_120_frame_windows).Count -ne 2){throw 'Frame gap was incorrectly bridged'}
    $rows=Get-Content -LiteralPath (Join-Path $profile 'v3-frame.csv')
    $rows[2]='2,1010,10,NaN,0,0'
    [IO.File]::WriteAllLines((Join-Path $profile 'v3-frame.csv'),$rows)
    & (Join-Path $PSScriptRoot 'OptimizedMW_Benchmark_Report.ps1') -ProfileDir $profile
    $bad=Get-Content -Raw -LiteralPath (Join-Path $profile 'Phase9-BENCHMARK-REPORT.json') | ConvertFrom-Json
    if($bad.malformed_frame_rows -ne 1 -or $bad.unfiltered.frames -ne 3 -or $bad.frame_pacing.contiguous_pairs -ne 1){throw 'Malformed row contaminated pacing'}
    Write-Host 'PASS frame pacing: equal-mean smooth/alternating subthreshold traces, rolling-baseline spike, local windows, gaps and malformed rows'
}finally{
    $resolved=[IO.Path]::GetFullPath($profile)
    if(-not $resolved.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Unexpected fixture cleanup path'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
