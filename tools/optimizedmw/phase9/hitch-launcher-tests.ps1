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
    'phase9_hitch_spans=0' | Set-Content -LiteralPath (Join-Path $profile 'TEST_MODE.txt')
    if($null -ne (Test-Phase9HitchCapture $profile).valid_span_capture){throw 'Unrequested trace treated as valid'}
    Write-Host 'PASS hitch qualification: real concurrent fixture, overflow, explicit truncation, footer, parent integrity, missing status and unarmed control'
}finally{
    $resolved=[IO.Path]::GetFullPath($profile)
    if(-not $resolved.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Unexpected fixture cleanup path'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
