# Post-run only; types share the validated HitchReport parser assembly.
. (Join-Path $PSScriptRoot 'OptimizedMW_HitchReport.ps1')

function Write-Phase9SpikeWorkReport {
    param([Parameter(Mandatory=$true)][string]$ProfileDir,[long[]]$ExtraFrames=@())
    $reasons=[Collections.Generic.List[string]]::new();$analysis=$null
    $timeline=Join-Path $ProfileDir 'p9-hitch-spans.csv'
    $modePath=Join-Path $ProfileDir 'TEST_MODE.txt'
    $requested=(Test-Path -LiteralPath $timeline) -or ((Test-Path -LiteralPath $modePath) -and
        ((Get-Content -Raw -LiteralPath $modePath) -match '(?m)^phase9_hitch_spans=1\r?$'))
    # Delete only this report's derived files, also when a previously present
    # raw timeline is now missing. Failed attempts must never retain old trees.
    foreach($name in @('HITCH-SPIKE-WORK-TREES.jsonl','HITCH-SPIKE-NODES.csv')){
        [IO.File]::Delete((Join-Path $ProfileDir $name))
    }
    if($requested){
        try{
            # Requalify current raw sources, including shutdown/status/clock
            # metadata. A saved success report cannot bless replaced evidence.
            $qualification=Test-Phase9HitchCapture -ProfileDir $ProfileDir
            if($qualification.valid_span_capture -ne $true){throw 'Work-tree joins require an independently valid CPU timeline'}
            $analysis=[Phase9.SpikeWorkReport]::Write($timeline,(Join-Path $ProfileDir 'Phase9-FRAME-PACING.csv'),$ProfileDir,$ExtraFrames)
            foreach($reason in $analysis.reasons){$reasons.Add($reason)}
        }catch{
            $analysis=$null;$reasons.Add([string]$_)
            foreach($name in @('HITCH-SPIKE-WORK-TREES.jsonl','HITCH-SPIKE-NODES.csv')){
                [IO.File]::Delete((Join-Path $ProfileDir $name))
            }
        }
    }
    $report=[pscustomobject]@{schema=1;requested=$requested;complete=$(if($requested){$reasons.Count -eq 0}else{$null});
        analysis=$analysis;reasons=$reasons.ToArray();
        selection='Every relative spike and >=25ms frame, plus selected positive local deviations. Nodes preserve original span IDs and same-thread parents.';
        interpretation='Worker overlap is concurrency, not proof it delayed the frame. Causal context includes dependencies outside the interval. Origin frame alone does not assign later worker time to that frame.';
        accounting='Frame anchors are intervals, not extra work. Main-thread envelope coverage is an interval union, not explained cost. Nested and parallel durations must not be summed. Self time for a span crossing a frame boundary cannot be allocated exactly to that frame.';
        node_files=@('HITCH-SPIKE-WORK-TREES.jsonl','HITCH-SPIKE-NODES.csv');scope='Engine wall intervals, not display presentation intervals. Fast filtered children remain partially unresolved.'}
    $report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $ProfileDir 'HITCH-SPIKE-REPORT.json') -Encoding UTF8
    return $report
}
