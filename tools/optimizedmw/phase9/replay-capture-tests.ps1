param([Parameter(Mandatory=$true)][string]$ProfilesRoot,
      [Parameter(Mandatory=$true)][string]$OutputDir)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'OptimizedMW_ProfileArchive.ps1')
$profiles=[IO.Path]::GetFullPath($ProfilesRoot)
$output=[IO.Path]::GetFullPath($OutputDir)
if($output.StartsWith($profiles,[StringComparison]::OrdinalIgnoreCase)){throw 'Replay output must be outside retained evidence'}
New-Item -ItemType Directory -Path $output -Force | Out-Null
$results=[Collections.Generic.List[object]]::new()
foreach($case in @(
    @('TEMPORAL-INPUTS_20260929_230535_979_167d0b',$false,6),
    @('TEMPORAL-TRACE_20260929_233039_640_342375',$true,0),
    @('TEMPORAL-OWNERSHIP_20260929_233851_331_db3289',$false,5))){
    $source=Join-Path $profiles ('OptimizedMW_Phase9_'+$case[0])
    $target=Join-Path $output $case[0]
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    $inventory=[ordered]@{}
    foreach($name in @('TEST_MODE.txt','p9-temporal-inputs.csv','p9-dlss-capabilities.csv',
        'p9-draw-phases.csv.status.txt','p9-draw-phases.csv.frames.csv',
        'p9-draw-phases.csv.gl-status.txt','p9-draw-phases.csv.resources.csv')){
        $file=Join-Path $source $name
        if(Test-Path -LiteralPath $file){
            $inventory[$name]=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
            Copy-Item -LiteralPath $file -Destination (Join-Path $target $name) -Force
        }
    }
    # Validators write only into the owned replay copy, never the raw profile.
    $temporal=Test-Phase9TemporalCapture -ProfileDir $target -TemporalRequested $true
    if($temporal.valid_temporal_capture -ne $case[1] -or $temporal.csv_writer_dropped -ne $case[2]){
        throw ('Legacy temporal integrity changed: '+$case[0])
    }
    $trace=$null
    if($case[0].StartsWith('TEMPORAL-TRACE')){
        $trace=Test-Phase9TraceCapture -ProfileDir $target -TraceRequested $true
        if($trace.valid_leaf_capture -ne $false -or $trace.stream_loss.resource_catalog_dropped_attempts -ne 30615){
            throw 'Recorded catalog overflow was incorrectly erased by larger new default'
        }
    }
    foreach($name in $inventory.Keys){
        if((Get-FileHash -LiteralPath (Join-Path $source $name) -Algorithm SHA256).Hash -ne $inventory[$name]){
            throw 'Retained evidence changed during read-only replay'
        }
    }
    [void]$results.Add([pscustomobject]@{run=$case[0];raw_input_sha256=$inventory;
        temporal=$temporal;trace=$trace;retained_evidence_unchanged=$true})
}
$results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'legacy-capture-parser-replay.json') -Encoding UTF8
Write-Host 'PASS: actual retained mode2/8 temporal loss remains invalid; mode4 complete temporal substream stays valid while recorded catalog overflow remains invalid; raw hashes unchanged.'
exit 0
