param([string]$StagedDir)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
foreach($name in @('OptimizedMW_Test.ps1','OptimizedMW_Benchmark_Report.ps1','OptimizedMW_ProfileArchive.ps1','launcher-tests.ps1')){
    $tokens=$null;$errors=$null
    $null=[System.Management.Automation.Language.Parser]::ParseFile((Join-Path $root $name),[ref]$tokens,[ref]$errors)
    if($errors.Count){throw ($errors | Out-String)}
}
$tokens=$null;$errors=$null
$ast=[System.Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'OptimizedMW_Test.ps1'),[ref]$tokens,[ref]$errors)
$fn=$ast.Find({param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Get-P9Mode'},$true)
if(-not $fn){throw 'Missing Phase 9 mode function'}
. ([scriptblock]::Create($fn.Extent.Text))
. (Join-Path $root 'OptimizedMW_ProfileArchive.ps1')
$expected=@(
    @('REFERENCE','0','0','0','0'),
    @('TEMPORAL-INPUTS','0','1','0','0'),
    @('ROOT-CAUSE-TRACE','0','0','0','1'),
    @('TEMPORAL-TRACE','0','1','0','1'),
    @('MOTION-VIEW','0','1','1','0'),
    @('HITCH','1','0','0','0'),
    @('HITCH-TRACE','1','0','0','1'))
for($i=0;$i -lt $expected.Count;$i++){
    $m=Get-P9Mode ([string]($i+1));$keys=@('Name','Stream','Temporal','View','Trace')
    for($k=0;$k -lt $keys.Count;$k++){if($m[$keys[$k]] -ne $expected[$i][$k]){throw 'Mode isolation changed'}}
    if($m.ContainsKey('Prewarm')){throw 'Rejected prewarm mode leaked back into launcher'}
}
$bad=$false;try{$null=Get-P9Mode '99'}catch{$bad=$true};if(-not $bad){throw 'Unknown mode accepted'}
$launcher=Get-Content -Raw -LiteralPath (Join-Path $root 'OptimizedMW_Test.ps1')
foreach($line in @("`$ResourceRepair='true'","`$LuaCache='true'","`$SoundWarm='true'","`$ShadowConsistency='true'")){
    if(-not $launcher.Contains($line)){throw 'P8U1 foundation differs between Phase 9 modes'}
}
if($launcher.Contains('OPENMW_P9_STATIC_PREWARM=$mode.Prewarm') -or $launcher.Contains('$mode.Prewarm')){
    throw 'Rejected static prewarm is still selectable'
}
foreach($token in @('OPENMW_P9_TEMPORAL_FILE','OPENMW_P9_DLSS_CAPS_FILE','OPENMW_P9_COMPOSITE_FILE',
                    'OPENMW_V36_GPU_PASS_FILE','phase9_temporal_contract=consumer_frame_v1',
                    'phase9_gl_vulkan_interop_probe=')){
    if(-not $launcher.Contains($token)){throw ('Missing Phase 9 continuation control: '+$token)}
}
if($launcher.IndexOf('Complete-Phase9Profile -ProfileDir') -lt $launcher.IndexOf('settings_restore_verified=$restoreVerified')){
    throw 'Packaging precedes configuration restoration'
}
$batPath=Join-Path $root '../gl-p8g4/START-OptimizedMW-Test.bat'
$bat=Get-Content -Raw -LiteralPath $batPath
$command=[regex]::Match($bat,'(?m)^powershell.exe .* -Command "(.*)"').Groups[1].Value
if(-not $command){throw 'Missing shader preflight'}
$tokens=$null;$errors=$null
$null=[System.Management.Automation.Language.Parser]::ParseInput($command,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Invalid BAT shader preflight'}
foreach($shader in @('temporal_camera_motion.vert','temporal_camera_motion.frag','temporal_motion_view.vert','temporal_motion_view.frag')){
    if(-not $command.Contains($shader)){throw "Unverified temporal shader: $shader"}
}
if($StagedDir){
    foreach($file in @('OptimizedMW_Test.ps1','OptimizedMW_Benchmark_Report.ps1','OptimizedMW_ProfileArchive.ps1','OptimizedMW-Phase9-README.txt')){
        if((Get-FileHash -LiteralPath (Join-Path $root $file)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $StagedDir $file)).Hash){
            throw ('Installed Phase 9 file mismatch: '+$file)
        }
    }
    Push-Location -LiteralPath $StagedDir
    try{& ([scriptblock]::Create($command))}finally{Pop-Location}
}
$temp=Join-Path ([IO.Path]::GetTempPath()) ('phase9 [archive] '+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
$zipFile="$temp.zip"
try{
    $header='frame,epoch_ms,wall_ms,input_ms,sound_ms,lua_sync_ms,state_ms,script_ms,mechanics_ms,physics_ms,world_ms,gui_ms,focus_ms,pre_viewer_ms,event_traversal_ms,update_traversal_ms,rendering_traversal_ms,lua_wait_ms,frame_limiter_ms,viewer_advance_ms,accounted_ms,other_ms'
    $rows=[Collections.Generic.List[string]]::new();[void]$rows.Add($header)
    $walls=@(10,30,12,40,15,150,10)
    for($i=0;$i -lt $walls.Count;$i++){$v=@(($i+1),1000,$walls[$i]);$v+=@(0)*17;$v+=@(0,$walls[$i]);[void]$rows.Add(($v -join ','))}
    [IO.File]::WriteAllLines((Join-Path $temp 'v3-frame.csv'),$rows)
    @('frame,name','2,"quoted,event"','7,adjacent','100,outside') | Set-Content -LiteralPath (Join-Path $temp 'v3-events.csv')
    'experiment=ARCHIVE-TEST' | Set-Content -LiteralPath (Join-Path $temp 'TEST_MODE.txt')
    'frame,total_ms' | Set-Content -LiteralPath (Join-Path $temp 'p6-render-traversal.csv')
    'fixture' | Set-Content -LiteralPath (Join-Path $temp 'openmw.log')
    $nested=Join-Path $temp 'nested';New-Item -ItemType Directory -Path $nested | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $nested 'binary.bin'),[byte[]]@(0,1,127,255))
    $zip=New-VerifiedProfileZip -SourceDir $temp -DestinationPath $zipFile
    if($zip.Missing.Count -ne 0){throw 'Complete archive incorrectly marked partial'}
    $before=(Get-FileHash -LiteralPath $zipFile).Hash
    $rejected=$false;try{$null=New-VerifiedProfileZip -SourceDir $temp -DestinationPath $zipFile}catch{$rejected=$true}
    if(-not $rejected -or (Get-FileHash -LiteralPath $zipFile).Hash -ne $before){throw 'Existing verified archive was clobbered'}
    # A report failing before ZIP creation was an earlier failure mode. Raw ZIP survives.
    $failScript=Join-Path $temp 'fail report.ps1'
    'param([string]$ProfileDir); throw "intentional report failure"' | Set-Content -LiteralPath $failScript
    $failed=$false;try{Invoke-BoundedOfflineReport -ReportScript $failScript -ProfileDir $temp -TimeoutSeconds 10}catch{$failed=$true}
    if(-not $failed -or (Get-FileHash -LiteralPath $zipFile).Hash -ne $before){throw 'Report failure lost raw ZIP'}
    $slowScript=Join-Path $temp 'slow report.ps1'
    'param([string]$ProfileDir); Start-Sleep -Seconds 30' | Set-Content -LiteralPath $slowScript
    $failed=$false;$clock=[Diagnostics.Stopwatch]::StartNew()
    try{Invoke-BoundedOfflineReport -ReportScript $slowScript -ProfileDir $temp -TimeoutSeconds 1}catch{$failed=$true}
    if(-not $failed -or $clock.Elapsed.TotalSeconds -gt 15 -or (Get-FileHash -LiteralPath $zipFile).Hash -ne $before){throw 'Report timeout guard failed'}
    & (Join-Path $root 'OptimizedMW_Benchmark_Report.ps1') -ProfileDir $temp
    $result=Get-Content -Raw -LiteralPath (Join-Path $temp 'Phase9-BENCHMARK-REPORT.json') | ConvertFrom-Json
    if($result.unfiltered.frames -ne 7 -or $result.unfiltered.median_ms -ne 15 -or [Math]::Abs($result.unfiltered.p95_ms-117) -gt .001){throw 'Frame statistics changed'}
    if($result.ordinary_under100.frames -ne 6 -or @($result.severe_frames_ge100).Count -ne 1 -or $result.malformed_frame_rows -ne 0){throw 'Severe/invalid frame handling changed'}
    if(@($result.clusters).Count -ne 1 -or $result.clusters[0].slow_frames -ne 3){throw 'Cluster rule changed'}
    $context=Get-Content -LiteralPath (Join-Path $temp 'cluster-context-v3-events.csv')
    if($context.Count -ne 3 -or $context[1] -ne '2,"quoted,event"'){throw 'Context lost quoting or +/-1 alignment'}
    $zip=New-VerifiedProfileZip -SourceDir $temp -DestinationPath $zipFile -ReplaceExisting
    $archive=[IO.Compression.ZipFile]::OpenRead($zipFile)
    try{if(-not $archive.GetEntry('Phase9-BENCHMARK-REPORT.json') -or -not $archive.GetEntry('nested/binary.bin')){throw 'Verified archive omitted report or nested binary'}}finally{$archive.Dispose()}
    # Crash/early failure evidence must still be packaged, not rejected for missing CSVs.
    Remove-Item -LiteralPath (Join-Path $temp 'v3-frame.csv')
    $zip=New-VerifiedProfileZip -SourceDir $temp -DestinationPath $zipFile -ReplaceExisting
    if($zip.Missing -notcontains 'v3-frame.csv'){throw 'Partial-capture provenance missing'}
    $bad=$false;try{$null=New-VerifiedProfileZip -SourceDir $temp -DestinationPath (Join-Path $temp 'bad.zip')}catch{$bad=$true}
    if(-not $bad){throw 'Self-containing archive accepted'}
    $badTrace=Test-Phase9TraceCapture -ProfileDir $temp -TraceRequested $true
    if($badTrace.valid_leaf_capture -ne $false){throw 'Empty runtime trace was accepted'}
    @('visitor_instances=2','valid_leaf_capture=1') | Set-Content -LiteralPath (Join-Path $temp 'p9-draw-phases.csv.status.txt')
    @('frame,context,leaves','1,0,4') | Set-Content -LiteralPath (Join-Path $temp 'p9-draw-phases.csv.frames.csv')
    $goodTrace=Test-Phase9TraceCapture -ProfileDir $temp -TraceRequested $true
    if($goodTrace.valid_leaf_capture -ne $true){throw 'Complete runtime trace rejected'}
    Write-Host 'PASS Phase 9: 7 safe isolated modes, rejected prewarm absent, live/empty trace validation, shader preflight, report fixtures, nested/spaced/bracket paths, real SHA256 ZIP verification, partial capture, report failure/timeout and verified replacement.'
}finally{
    if(Test-Path -LiteralPath $zipFile){Remove-Item -LiteralPath $zipFile -Force}
    Remove-Item -LiteralPath $temp -Recurse -Force
}
