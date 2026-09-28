param([string]$StagedDir)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
foreach($name in @('OptimizedMW_Test.ps1','OptimizedMW_Benchmark_Report.ps1','launcher-tests.ps1')){
    $tokens=$null;$errors=$null
    $null=[System.Management.Automation.Language.Parser]::ParseFile((Join-Path $root $name),[ref]$tokens,[ref]$errors)
    if($errors.Count){throw ($errors | Out-String)}
}
$bat=Get-Content -Raw (Join-Path $root 'START-OptimizedMW-Test.bat')
$command=[regex]::Match($bat,'(?m)^powershell.exe .* -Command "(.*)"').Groups[1].Value
if(-not $command){throw 'Missing shader preflight'}
$tokens=$null;$errors=$null
$null=[System.Management.Automation.Language.Parser]::ParseInput($command,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Invalid BAT preflight'}
if($StagedDir){Push-Location $StagedDir;try{& ([scriptblock]::Create($command))}finally{Pop-Location}}
$temp=Join-Path ([IO.Path]::GetTempPath()) ('p8g4-report-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try{
    $header='frame,epoch_ms,wall_ms,input_ms,sound_ms,lua_sync_ms,state_ms,script_ms,mechanics_ms,physics_ms,world_ms,gui_ms,focus_ms,pre_viewer_ms,event_traversal_ms,update_traversal_ms,rendering_traversal_ms,lua_wait_ms,frame_limiter_ms,viewer_advance_ms,accounted_ms,other_ms'
    $rows=[Collections.Generic.List[string]]::new();[void]$rows.Add($header)
    $walls=@(10,30,12,40,15,150,10)
    for($i=0;$i -lt $walls.Count;$i++){
        $values=@(($i+1),1000,$walls[$i]);$values+=@(0)*17;$values+=@(0,$walls[$i]);[void]$rows.Add(($values -join ','))
    }
    [IO.File]::WriteAllLines((Join-Path $temp 'v3-frame.csv'),$rows)
    @('frame,name','2,"quoted,event"','7,adjacent','100,outside') | Set-Content (Join-Path $temp 'v3-events.csv')
    & (Join-Path $root 'OptimizedMW_Benchmark_Report.ps1') -ProfileDir $temp
    $result=Get-Content -Raw (Join-Path $temp 'P8G4-BENCHMARK-REPORT.json') | ConvertFrom-Json
    if($result.unfiltered.frames -ne 7 -or $result.unfiltered.median_ms -ne 15){throw 'Full-capture metrics incorrect'}
    if([Math]::Abs($result.unfiltered.p95_ms-117) -gt .001){throw 'Percentile interpolation incorrect'}
    if($result.ordinary_under100.frames -ne 6){throw 'Ordinary/unfiltered separation incorrect'}
    if(@($result.clusters).Count -ne 1 -or $result.clusters[0].slow_frames -ne 3){throw 'Cluster bridge rule incorrect'}
    if(@($result.severe_frames_ge100).Count -ne 1){throw 'Severe events lost'}
    if($result.malformed_frame_rows -ne 0){throw 'Valid fixture counted malformed'}
    $context=Get-Content (Join-Path $temp 'cluster-context-v3-events.csv')
    if($context.Count -ne 3 -or $context[1] -ne '2,"quoted,event"'){throw 'Context lost CSV quoting or +/-1 frame alignment'}
    Write-Host 'P8G4 launcher syntax, preflight and offline report fixture passed.'
}finally{Remove-Item -Recurse -Force -LiteralPath $temp}
