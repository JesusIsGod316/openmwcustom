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
foreach($choice in @('8','9','10','11','12')){
    $mode=Get-P9Mode $choice
    if($mode.Ownership -ne $(if($choice -eq '9'){'0'}else{'1'})){throw 'Ownership control isolation changed'}
    if($mode.Composite -ne $(if($choice -in @('9','10','12')){'1'}else{'0'})){throw 'Composite control isolation changed'}
    if($mode.DynamicMotion -ne $(if($choice -eq '11'){'1'}else{'0'})){throw 'Dynamic motion control isolation changed'}
}
$launcher=Get-Content -Raw -LiteralPath (Join-Path $root 'OptimizedMW_Test.ps1')
foreach($line in @("`$ResourceRepair='true'","`$LuaCache='true'","`$SoundWarm='true'","`$ShadowConsistency='true'")){
    if(-not $launcher.Contains($line)){throw 'P8U1 foundation differs between Phase 9 modes'}
}
if($launcher.Contains('OPENMW_P9_STATIC_PREWARM=$mode.Prewarm') -or $launcher.Contains('$mode.Prewarm')){
    throw 'Rejected static prewarm is still selectable'
}
foreach($token in @('OPENMW_P9_TEMPORAL_FILE','OPENMW_P9_DLSS_CAPS_FILE','OPENMW_P9_COMPOSITE_FILE',
                    'OPENMW_V36_GPU_PASS_FILE','phase9_temporal_contract=consumer_frame_v2_ownership',
                    'OPENMW_P9_CAPTURE_TRANSPORT','OPENMW_P9_RESOURCE_CAPACITY',
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
foreach($shader in @('temporal_camera_motion.vert','temporal_camera_motion.frag','temporal_motion_view.vert','temporal_motion_view.frag','temporal_dynamic_motion.vert','temporal_dynamic_motion.frag')){
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
    [void]$rows.Add('# v3_async_diagnostics_dropped_lines=0')
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
    $successScript=Join-Path $temp 'successful report.ps1'
    'param([string]$ProfileDir); [IO.File]::WriteAllText((Join-Path $ProfileDir "child-success.txt"),"complete"); exit 0' |
        Set-Content -LiteralPath $successScript
    Invoke-BoundedOfflineReport -ReportScript $successScript -ProfileDir $temp -TimeoutSeconds 10
    if(-not (Test-Path -LiteralPath (Join-Path $temp 'child-success.txt'))){throw 'Successful child exit was incorrectly rejected'}
    # A report failing before ZIP creation was an earlier failure mode. Raw ZIP survives.
    $failScript=Join-Path $temp 'fail report.ps1'
    'param([string]$ProfileDir); throw "intentional report failure"' | Set-Content -LiteralPath $failScript
    $failed=$false;$failureText='';try{Invoke-BoundedOfflineReport -ReportScript $failScript -ProfileDir $temp -TimeoutSeconds 10}catch{$failed=$true;$failureText=[string]$_}
    if(-not $failed -or $failureText -notmatch 'exited with code 1;' -or (Get-FileHash -LiteralPath $zipFile).Hash -ne $before){throw 'Report failure exit code or raw ZIP preservation failed'}
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
    $traceStatus=@('visitor_instances=2','valid_leaf_capture=1','rows_dropped=0','frame_rows_dropped=0',
        'renderer_rows_dropped=0','uninstrumented_pool_overflow=0','resource_catalog_dropped_attempts=0','resource_catalog_rows=1')
    $traceStatus | Set-Content -LiteralPath (Join-Path $temp 'p9-draw-phases.csv.status.txt')
    @('rows_dropped=0','frame_rows_dropped=0') | Set-Content -LiteralPath (Join-Path $temp 'p9-draw-phases.csv.gl-status.txt')
    @('frame,context,leaves','1,0,4') | Set-Content -LiteralPath (Join-Path $temp 'p9-draw-phases.csv.frames.csv')
    $resourcePath=Join-Path $temp 'p9-draw-phases.csv.resources.csv'
    $resourceHeader='first_frame,last_frame,context,texture_unit,texture,image,image_revision,stateset,submit_camera,bytes,scope,texture_class,filename,semantic_role'
    $resourceParts=@('1','9','4294967295','3','18446744073709551615','123','4','456','0','16','2',
        '"PreparedTerrainTexture"','"terrain,""stone"".dds"','"terrain_composite_diffuse"')
    $resourceRow=$resourceParts -join ','
    @($resourceHeader,$resourceRow) | Set-Content -LiteralPath $resourcePath
    $goodTrace=Test-Phase9TraceCapture -ProfileDir $temp -TraceRequested $true
    if($goodTrace.valid_leaf_capture -ne $true -or $goodTrace.resource_catalog_rows -ne 1){throw 'Complete runtime trace/unsigned pointer/unknown catalog context rejected'}
    @($traceStatus)+@('resource_catalog_capacity=131072','resource_catalog_allocation_failures=0') |
        Set-Content -LiteralPath (Join-Path $temp 'p9-draw-phases.csv.status.txt')
    $expanded=Test-Phase9TraceCapture -ProfileDir $temp -TraceRequested $true
    if(-not $expanded.valid_leaf_capture -or $expanded.resource_catalog_capacity -ne 131072){throw 'Declared bounded catalog capacity was ignored'}
    # Exceed the historical cap with genuine distinct descriptor identities.
    $writer=[IO.StreamWriter]::new($resourcePath,$false)
    try{
        $writer.WriteLine($resourceHeader)
        for($index=1;$index -le 32769;$index++){
            $writer.WriteLine(('1,9,4294967295,3,{0},123,4,456,0,16,2,"Texture2D","fixture.dds","terrain_composite_diffuse"' -f $index))
        }
    }finally{$writer.Dispose()}
    $expandedStatus=@($traceStatus | ForEach-Object {if($_ -eq 'resource_catalog_rows=1'){'resource_catalog_rows=32769'}else{$_}})
    @($expandedStatus)+@('resource_catalog_capacity=131072','resource_catalog_allocation_failures=0') |
        Set-Content -LiteralPath (Join-Path $temp 'p9-draw-phases.csv.status.txt')
    if(-not (Test-Phase9TraceCapture -ProfileDir $temp -TraceRequested $true).valid_leaf_capture){throw 'New bounded catalog larger than32768 was incorrectly rejected'}
    $expandedStatus | Set-Content -LiteralPath (Join-Path $temp 'p9-draw-phases.csv.status.txt')
    if((Test-Phase9TraceCapture -ProfileDir $temp -TraceRequested $true).valid_leaf_capture){throw 'Legacy capture silently escaped its old bound'}
    @($resourceHeader,$resourceRow) | Set-Content -LiteralPath $resourcePath
    foreach($capacity in @('-1','262145','invalid')){
        @($traceStatus)+@(('resource_catalog_capacity='+$capacity),'resource_catalog_allocation_failures=0') |
            Set-Content -LiteralPath (Join-Path $temp 'p9-draw-phases.csv.status.txt')
        if((Test-Phase9TraceCapture -ProfileDir $temp -TraceRequested $true).valid_leaf_capture){throw 'Invalid or unbounded catalog capacity accepted'}
    }
    @($traceStatus)+@('resource_catalog_capacity=131072','resource_catalog_allocation_failures=1') |
        Set-Content -LiteralPath (Join-Path $temp 'p9-draw-phases.csv.status.txt')
    if((Test-Phase9TraceCapture -ProfileDir $temp -TraceRequested $true).valid_leaf_capture){throw 'Catalog allocation failure accepted'}
    $traceStatus | Set-Content -LiteralPath (Join-Path $temp 'p9-draw-phases.csv.status.txt')
    # A texture-free scene legitimately emits only the named catalog header.
    $resourceHeader | Set-Content -LiteralPath $resourcePath
    @($traceStatus | ForEach-Object {if($_ -eq 'resource_catalog_rows=1'){'resource_catalog_rows=0'}else{$_}}) |
        Set-Content -LiteralPath (Join-Path $temp 'p9-draw-phases.csv.status.txt')
    $emptyCatalog=Test-Phase9TraceCapture -ProfileDir $temp -TraceRequested $true
    if($emptyCatalog.valid_leaf_capture -ne $true -or $emptyCatalog.resource_catalog_rows -ne 0){throw 'Texture-free header-only catalog rejected'}
    $traceStatus | Set-Content -LiteralPath (Join-Path $temp 'p9-draw-phases.csv.status.txt')
    @('semantic_role,filename,texture_class,scope,bytes,submit_camera,stateset,image_revision,image,texture,texture_unit,context,last_frame,first_frame',
        '"terrain_composite_diffuse","terrain,""stone"".dds","PreparedTerrainTexture",2,16,0,456,4,123,18446744073709551615,3,4294967295,9,1') |
        Set-Content -LiteralPath $resourcePath
    $namedCatalog=Test-Phase9TraceCapture -ProfileDir $temp -TraceRequested $true
    if($namedCatalog.valid_leaf_capture -ne $true){throw 'Catalog named columns or quoted filename parsing changed'}
    $boundaryParts=$resourceParts.Clone()
    foreach($index in @(0,1,3,6)){$boundaryParts[$index]='4294967295'}
    @($resourceHeader,($boundaryParts -join ',')) | Set-Content -LiteralPath $resourcePath
    $boundaries=Test-Phase9TraceCapture -ProfileDir $temp -TraceRequested $true
    if($boundaries.valid_leaf_capture -ne $true){throw 'Inclusive unsigned32 catalog bounds rejected'}
    Remove-Item -LiteralPath $resourcePath
    $missingCatalog=Test-Phase9TraceCapture -ProfileDir $temp -TraceRequested $true
    if($missingCatalog.valid_leaf_capture -ne $false){throw 'Missing resource catalog accepted'}
    foreach($badHeader in @($resourceHeader.Replace('semantic_role','other'),$resourceHeader.Replace('semantic_role','filename'))){
        @($badHeader,$resourceRow) | Set-Content -LiteralPath $resourcePath
        $bad=Test-Phase9TraceCapture -ProfileDir $temp -TraceRequested $true
        if($bad.valid_leaf_capture -ne $false){throw 'Missing/duplicate resource catalog column accepted'}
    }
    foreach($badCase in @(@(0,'-1'),@(0,'4294967296'),@(1,'0'),@(2,'16'),@(2,'-1'),@(2,'4294967296'),
        @(3,'-1'),@(3,'4294967296'),@(4,'0'),@(4,'18446744073709551616'),@(5,'-1'),@(5,'0'),
        @(6,'4294967296'),@(7,'0'),@(8,'-1'),@(8,'18446744073709551616'),@(9,'-1'),
        @(9,'18446744073709551616'),@(10,'3'),@(11,'""'),@(13,'""'))){
        $parts=$resourceParts.Clone();$parts[$badCase[0]]=$badCase[1]
        @($resourceHeader,($parts -join ',')) | Set-Content -LiteralPath $resourcePath
        $bad=Test-Phase9TraceCapture -ProfileDir $temp -TraceRequested $true
        if($bad.valid_leaf_capture -ne $false -or $bad.resource_catalog_incoherent_rows -ne 1){throw 'Resource catalog numeric bound/coherence failure accepted'}
    }
    foreach($badRow in @(($resourceRow+',extra'),$resourceRow.Substring(0,$resourceRow.LastIndexOf(',')),($resourceRow+'"'))){
        @($resourceHeader,$badRow) | Set-Content -LiteralPath $resourcePath
        $bad=Test-Phase9TraceCapture -ProfileDir $temp -TraceRequested $true
        if($bad.valid_leaf_capture -ne $false){throw 'Malformed catalog field count/quoting accepted'}
    }
    @($resourceHeader,$resourceRow,'# v3_async_diagnostics_dropped_lines=3') | Set-Content -LiteralPath $resourcePath
    $lostCatalog=Test-Phase9TraceCapture -ProfileDir $temp -TraceRequested $true
    if($lostCatalog.valid_leaf_capture -ne $false){throw 'Catalog CSV loss accepted'}
    @($resourceHeader,$resourceRow,$resourceRow) | Set-Content -LiteralPath $resourcePath
    $mismatchedCatalog=Test-Phase9TraceCapture -ProfileDir $temp -TraceRequested $true
    if($mismatchedCatalog.valid_leaf_capture -ne $false){throw 'Catalog/status row-count mismatch accepted'}
    @($resourceHeader,$resourceRow) | Set-Content -LiteralPath $resourcePath
    foreach($field in @('resource_catalog_dropped_attempts','renderer_rows_dropped')){
        @($traceStatus | ForEach-Object {if($_ -eq ($field+'=0')){$field+'=3'}else{$_}}) |
            Set-Content -LiteralPath (Join-Path $temp 'p9-draw-phases.csv.status.txt')
        $lost=Test-Phase9TraceCapture -ProfileDir $temp -TraceRequested $true
        if($lost.valid_leaf_capture -ne $false -or $lost.stream_loss[$field] -ne 3){throw 'Catalog/outer trace loss was hidden by valid leaf status'}
    }
    $traceStatus | Set-Content -LiteralPath (Join-Path $temp 'p9-draw-phases.csv.status.txt')
    @('rows_dropped=2','frame_rows_dropped=0') | Set-Content -LiteralPath (Join-Path $temp 'p9-draw-phases.csv.gl-status.txt')
    $lost=Test-Phase9TraceCapture -ProfileDir $temp -TraceRequested $true
    if($lost.valid_leaf_capture -ne $false -or $lost.stream_loss.gl_rows_dropped -ne 2){throw 'Selected GL trace loss ignored'}

    $temporalHeader='dlss_ready,frame,context,submitted,history_valid,previous_frame,render_w,render_h,output_w,output_h,color_ptr,depth_ptr,motion_ptr,input_mask,writer_dropped_total'
    $temporalRow='0,2,0,1,1,1,32,32,64,64,123,456,789,63,0'
    @($temporalHeader,$temporalRow,'# v3_async_diagnostics_dropped_lines=0') | Set-Content -LiteralPath (Join-Path $temp 'p9-temporal-inputs.csv')
    @('context,gl_vulkan_bridge_candidate','0,1') | Set-Content -LiteralPath (Join-Path $temp 'p9-dlss-capabilities.csv')
    $goodTemporal=Test-Phase9TemporalCapture -ProfileDir $temp -TemporalRequested $true
    if(($goodTemporal.valid_temporal_capture -ne $true) -or
       ($goodTemporal.temporal_input_rows -ne 1) -or
       ($goodTemporal.interop_capability_rows -ne 1)){
        throw 'Complete temporal capture rejected'
    }
    @($temporalHeader,('1'+$temporalRow.Substring(1))) | Set-Content -LiteralPath (Join-Path $temp 'p9-temporal-inputs.csv')
    $badTemporal=Test-Phase9TemporalCapture -ProfileDir $temp -TemporalRequested $true
    if($badTemporal.valid_temporal_capture -ne $false -or $badTemporal.unexpected_dlss_ready_rows -ne 1){
        throw 'Premature DLSS-ready telemetry was accepted'
    }
    @($temporalHeader,$temporalRow,'# v3_async_diagnostics_dropped_lines=3') | Set-Content -LiteralPath (Join-Path $temp 'p9-temporal-inputs.csv')
    $loss=Test-Phase9TemporalCapture -ProfileDir $temp -TemporalRequested $true
    if($loss.valid_temporal_capture -ne $false -or $loss.csv_writer_dropped -ne 3 -or $loss.temporal_input_rows -ne 1){throw 'Footer loss was counted as a frame or ignored'}
    @($temporalHeader,($temporalRow.Replace('1,1,32','1,2,32'))) | Set-Content -LiteralPath (Join-Path $temp 'p9-temporal-inputs.csv')
    $incoherent=Test-Phase9TemporalCapture -ProfileDir $temp -TemporalRequested $true
    if($incoherent.valid_temporal_capture -ne $false -or $incoherent.incoherent_rows -ne 1){throw 'Future/repeated history accepted'}
    foreach($badRow in @($temporalRow.Replace(',63,0',',-1,0'),$temporalRow.Replace(',63,0',',31,0'),
        $temporalRow.Replace('1,1,32','1,-1,32'),$temporalRow.Replace('0,2,0','0,4,0'))){
        @($temporalHeader,$badRow) | Set-Content -LiteralPath (Join-Path $temp 'p9-temporal-inputs.csv')
        $bad=Test-Phase9TemporalCapture -ProfileDir $temp -TemporalRequested $true
        if($bad.valid_temporal_capture -ne $false -or $bad.incoherent_rows -ne 1){throw 'Negative mask/frame, history mask mismatch or skipped history accepted'}
    }

    # New captures must prove both transport completion and effective ownership;
    # old loss-free temporal streams remain independently valid above.
    function Set-FixtureWriterCsv {
        param([string]$Name,[string[]]$Rows,[int]$Lost=0,[int]$OutputOk=1)
        @($Rows)+@(('# v3_async_diagnostics_dropped_lines='+$Lost),'# p9_capture_transport=bounded_mpsc_v1') |
            Set-Content -LiteralPath (Join-Path $temp $Name)
        @('normal_finish=1',('output_ok='+$OutputOk),('rows_dropped='+$Lost),'queue_capacity=32768',
            'byte_capacity=33554432','row_capacity=65536',('capacity_dropped='+$Lost),'contention_dropped=0',
            'oversized_dropped=0','allocation_dropped=0','io_dropped=0','stopped_dropped=0','initialization_failed=0') |
            Set-Content -LiteralPath (Join-Path $temp ($Name+'.writer-status.txt'))
    }
    @('phase9_capture_transport=bounded_mpsc_v1','phase9_temporal_contract=consumer_frame_v2_ownership',
        'phase9_temporal_inputs=1','phase9_temporal_ownership=1','phase9_draw_trace=0') |
        Set-Content -LiteralPath (Join-Path $temp 'TEST_MODE.txt')
    $ownedHeader=$temporalHeader+',ownership_requested,ownership_active,ownership_fallback_reason'
    $ownedRow=$temporalRow+',1,1,active'
    Set-FixtureWriterCsv 'p9-temporal-inputs.csv' @($ownedHeader,$ownedRow)
    Set-FixtureWriterCsv 'p9-dlss-capabilities.csv' @('context,gl_vulkan_bridge_candidate','0,1')
    Set-FixtureWriterCsv 'p9-gpu-passes.csv' @('context,pass','0,fixture')
    Set-FixtureWriterCsv 'v3-render.csv' @('frame,phase','1,fixture')
    Set-FixtureWriterCsv 'v3-events.csv' @('frame,event','1,fixture')
    Set-FixtureWriterCsv 'p6-render-traversal.csv' @('frame,total_ms','1,1')
    $owned=Test-Phase9TemporalCapture -ProfileDir $temp -TemporalRequested $true
    if(-not $owned.valid_temporal_capture -or -not $owned.valid_ownership_capture -or $owned.ownership_active_rows -ne 1){
        throw 'Actual active ownership or bounded writer completion was not accepted'
    }
    $writers=Test-Phase9WriterCapture -ProfileDir $temp
    if(-not $writers.valid_writer_capture){throw 'Complete bounded channels were incorrectly rejected'}
    Set-FixtureWriterCsv 'p9-temporal-inputs.csv' @($ownedHeader,($temporalRow+',1,0,unsupported_fx_state'))
    $fallback=Test-Phase9TemporalCapture -ProfileDir $temp -TemporalRequested $true
    if(-not $fallback.valid_temporal_capture -or $fallback.valid_ownership_capture -ne $false -or
        $fallback.ownership_fallbacks.unsupported_fx_state -ne 1){throw 'Effective fallback was promoted or lost coherent temporal evidence'}
    foreach($suffix in @(',0,1,active',',1,0,active',',1,0,unknown_reason',',0,0,not_requested')){
        Set-FixtureWriterCsv 'p9-temporal-inputs.csv' @($ownedHeader,($temporalRow+$suffix))
        if((Test-Phase9TemporalCapture -ProfileDir $temp -TemporalRequested $true).valid_temporal_capture){
            throw 'Incoherent ownership activation, request flag or fallback reason accepted'
        }
    }
    Set-FixtureWriterCsv 'p9-temporal-inputs.csv' @($temporalHeader,$temporalRow)
    if((Test-Phase9TemporalCapture -ProfileDir $temp -TemporalRequested $true).valid_temporal_capture){throw 'New launcher accepted missing ownership columns'}
    Set-FixtureWriterCsv 'p9-temporal-inputs.csv' @($ownedHeader,$ownedRow)
    Set-FixtureWriterCsv 'p9-gpu-passes.csv' @('context,pass','0,fixture') -Lost 3
    $writers=Test-Phase9WriterCapture -ProfileDir $temp
    $temporal=Test-Phase9TemporalCapture -ProfileDir $temp -TemporalRequested $true
    if($writers.valid_writer_capture -ne $false -or $writers.csv_writer_dropped -ne 3 -or -not $temporal.valid_temporal_capture){
        throw 'GPU loss hidden, or independent complete temporal evidence was erased'
    }
    Set-FixtureWriterCsv 'p9-temporal-inputs.csv' @($ownedHeader,$ownedRow) -OutputOk 0
    if((Test-Phase9TemporalCapture -ProfileDir $temp -TemporalRequested $true).valid_temporal_capture){throw 'Failed writer output was accepted'}
    Set-FixtureWriterCsv 'p9-temporal-inputs.csv' @($ownedHeader,$ownedRow)
    Remove-Item -LiteralPath (Join-Path $temp 'p9-temporal-inputs.csv.writer-status.txt')
    if((Test-Phase9TemporalCapture -ProfileDir $temp -TemporalRequested $true).valid_temporal_capture){throw 'Missing bounded writer finish was accepted'}
    [IO.File]::WriteAllLines((Join-Path $temp 'v3-frame.csv'),$rows)
    & (Join-Path $root 'OptimizedMW_Benchmark_Report.ps1') -ProfileDir $temp
    $qualifiedReport=Get-Content -Raw -LiteralPath (Join-Path $temp 'Phase9-BENCHMARK-REPORT.json') | ConvertFrom-Json
    if($qualifiedReport.unfiltered.frames -ne 7 -or
        $qualifiedReport.channel_qualifications.'WRITER-CAPTURE.json'.valid_writer_capture -ne $false -or
        $qualifiedReport.channel_qualifications.'WRITER-CAPTURE.json'.csv_writer_dropped -ne 3){
        throw 'Offline report hid independent writer loss or erased CPU statistics'
    }

    Write-Host 'PASS Phase 9: isolated modes, dynamic bounded catalog/allocation validation, legacy/new ownership semantics, writer completion/output/footer/loss, independent GPU/temporal integrity, fail-closed DLSS, shader preflight, report success/failure exit codes and timeout, verified raw ZIP/partial capture/replacement.'
}finally{
    if(Test-Path -LiteralPath $zipFile){Remove-Item -LiteralPath $zipFile -Force}
    Remove-Item -LiteralPath $temp -Recurse -Force
}
# Expected report failures leave a nonzero native child exit code in the host.
# Return success only after all assertions and cleanup have completed.
exit 0
