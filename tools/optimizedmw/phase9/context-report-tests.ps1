$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'OptimizedMW_ContextReport.ps1')
$tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixture=Join-Path $tempRoot ('p9-context-report-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
try{
    [IO.File]::WriteAllLines((Join-Path $fixture 'v3-frame.csv'),@(
        'frame,epoch_ms,wall_ms','1,1,10','2,2,90','3,3,500','4,4,100','5,5,12','6,6,20'))
    [IO.File]::WriteAllLines((Join-Path $fixture 'benchmark-context.csv'),@(
        'frame,viewer_end_frame,begin_flags,end_flags,observed_flags',
        '1,1,0,0,0','2,2,0,0,0','3,30,0,0,32','4,4,0,0,3','5,5,0,0,0','6,6,0,0,0'))
    $statusPath=Join-Path $fixture 'benchmark-context.csv.capture-status.txt'
    $status=@('schema=1','normal_finish=1','rows=6','snapshot_rows=1','dropped=0','snapshots_dropped=0',
        'unfinished_frames=0','allocation_failed=0','output_ok=1')
    [IO.File]::WriteAllLines($statusPath,$status)
    $result=Get-OptimizedMWContextReport $fixture
    if(-not $result.qualified -or $result.observed_state_cohorts.cohorts['observed_active']['frames'] -ne 4 -or
        $result.observed_state_cohorts.cohorts['observed_active']['max_ms'] -ne 90 -or
        $result.observed_state_cohorts.cohorts['observed_active']['consecutive_pairs'] -ne 2 -or
        $result.observed_state_cohorts.cohorts['loading_or_viewer_advance']['max_ms'] -ne 500 -or
        $result.observed_state_cohorts.cohorts['paused']['max_ms'] -ne 100){throw 'State cohort lost slow/transition/adjacent frames'}
    $status[4]='dropped=1';[IO.File]::WriteAllLines($statusPath,$status)
    if((Get-OptimizedMWContextReport $fixture).qualified){throw 'Lost state capture qualified'}
    $status[4]='dropped=0';[IO.File]::WriteAllLines($statusPath,$status)
    $framePath=Join-Path $fixture 'v3-frame.csv'
    $originalFrames=[IO.File]::ReadAllText($framePath)
    Add-Content -LiteralPath $framePath -Value '2,2,90'
    $duplicate=Get-OptimizedMWContextReport $fixture
    if($duplicate.qualified -or $duplicate.reason -notmatch 'Duplicate frame timing ID'){throw 'Duplicate timing frame qualified'}
    [IO.File]::WriteAllText($framePath,$originalFrames)
    Add-Content -LiteralPath (Join-Path $fixture 'v3-frame.csv') -Value '7,7,80'
    $missing=Get-OptimizedMWContextReport $fixture
    if($missing.qualified -or $missing.observed_state_cohorts.missing_state_frames -ne 1 -or
        $missing.observed_state_cohorts.cohorts['unknown_state']['max_ms'] -ne 80){throw 'Unknown-state slow frame hidden or qualified'}
    Add-Content -LiteralPath (Join-Path $fixture 'benchmark-context.csv') -Value '1,1,0,0,0'
    if((Get-OptimizedMWContextReport $fixture).qualified){throw 'Duplicate state frame qualified'}
    Write-Host 'PASS explicit state joins, uncensored slow/transition frames, transient pause, adjacency, losses and unknown records'
}finally{
    $resolved=[IO.Path]::GetFullPath($fixture)
    if(-not $resolved.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notlike 'p9-context-report-*'){throw 'Unexpected context fixture cleanup'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
